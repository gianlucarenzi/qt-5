// Copyright 2014 PDFium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/edit/cpdf_creator.h"

#include <algorithm>

#include "core/fpdfapi/edit/cpdf_encryptor.h"
#include "core/fpdfapi/edit/cpdf_flateencoder.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_crypto_handler.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_security_handler.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fxcrt/fx_extension.h"
#include "core/fxcrt/fx_random.h"
#include "third_party/base/span.h"

namespace {

const size_t kArchiveBufferSize = 32768;

class CFX_FileBufferArchive : public IFX_ArchiveStream {
 public:
  explicit CFX_FileBufferArchive(const RetainPtr<IFX_WriteStream>& archive);
  ~CFX_FileBufferArchive() override;

  bool WriteBlock(const void* pBuf, size_t size) override;
  bool WriteByte(uint8_t byte) override;
  bool WriteDWord(uint32_t i) override;
  bool WriteString(const ByteStringView& str) override;

  FX_FILESIZE CurrentOffset() const override { return offset_; }

 private:
  bool Flush();

  FX_FILESIZE offset_;
  size_t current_length_;
  std::vector<uint8_t> buffer_;
  RetainPtr<IFX_WriteStream> backing_file_;
};

CFX_FileBufferArchive::CFX_FileBufferArchive(
    const RetainPtr<IFX_WriteStream>& file)
    : offset_(0),
      current_length_(0),
      buffer_(kArchiveBufferSize),
      backing_file_(file) {
  ASSERT(file);
}

CFX_FileBufferArchive::~CFX_FileBufferArchive() {
  Flush();
}

bool CFX_FileBufferArchive::Flush() {
  size_t nRemaining = current_length_;
  current_length_ = 0;
  if (!backing_file_)
    return false;
  if (!nRemaining)
    return true;
  return backing_file_->WriteBlock(buffer_.data(), nRemaining);
}

bool CFX_FileBufferArchive::WriteBlock(const void* pBuf, size_t size) {
  ASSERT(pBuf && size > 0);

  const uint8_t* buffer = reinterpret_cast<const uint8_t*>(pBuf);
  size_t temp_size = size;
  while (temp_size) {
    size_t buf_size = std::min(kArchiveBufferSize - current_length_, temp_size);
    memcpy(buffer_.data() + current_length_, buffer, buf_size);

    current_length_ += buf_size;
    if (current_length_ == kArchiveBufferSize && !Flush())
      return false;

    temp_size -= buf_size;
    buffer += buf_size;
  }

  pdfium::base::CheckedNumeric<FX_FILESIZE> safe_offset = offset_;
  safe_offset += size;
  if (!safe_offset.IsValid())
    return false;

  offset_ = safe_offset.ValueOrDie();
  return true;
}

bool CFX_FileBufferArchive::WriteByte(uint8_t byte) {
  return WriteBlock(&byte, 1);
}

bool CFX_FileBufferArchive::WriteDWord(uint32_t i) {
  char buf[32];
  FXSYS_itoa(i, buf, 10);
  return WriteBlock(buf, strlen(buf));
}

bool CFX_FileBufferArchive::WriteString(const ByteStringView& str) {
  return WriteBlock(str.raw_str(), str.GetLength());
}

std::vector<uint8_t> GenerateFileID(uint32_t dwSeed1, uint32_t dwSeed2) {
  std::vector<uint8_t> buffer(sizeof(uint32_t) * 4);
  uint32_t* pBuffer = reinterpret_cast<uint32_t*>(buffer.data());
  void* pContext = FX_Random_MT_Start(dwSeed1);
  for (int i = 0; i < 2; ++i)
    *pBuffer++ = FX_Random_MT_Generate(pContext);

  FX_Random_MT_Close(pContext);
  pContext = FX_Random_MT_Start(dwSeed2);
  for (int i = 0; i < 2; ++i)
    *pBuffer++ = FX_Random_MT_Generate(pContext);

  FX_Random_MT_Close(pContext);
  return buffer;
}

int32_t OutputIndex(IFX_ArchiveStream* archive, FX_FILESIZE offset) {
  if (!archive->WriteByte(static_cast<uint8_t>(offset >> 24)) ||
      !archive->WriteByte(static_cast<uint8_t>(offset >> 16)) ||
      !archive->WriteByte(static_cast<uint8_t>(offset >> 8)) ||
      !archive->WriteByte(static_cast<uint8_t>(offset)) ||
      !archive->WriteByte(0)) {
    return -1;
  }
  return 0;
}

}  // namespace

CPDF_Creator::CPDF_Creator(CPDF_Document* pDoc,
                           const RetainPtr<IFX_WriteStream>& archive)
    : m_pDocument(pDoc),
      m_pParser(pDoc->GetParser()),
      m_pEncryptDict(m_pParser ? m_pParser->GetEncryptDict() : nullptr),
      m_pSecurityHandler(m_pParser ? m_pParser->GetSecurityHandler() : nullptr),
      m_dwLastObjNum(m_pDocument->GetLastObjNum()),
      m_Archive(pdfium::MakeUnique<CFX_FileBufferArchive>(archive)) {}

CPDF_Creator::~CPDF_Creator() {}

bool CPDF_Creator::WriteStream(const CPDF_Object* pStream, uint32_t objnum) {
  CPDF_CryptoHandler* pCrypto =
      pStream != m_pMetadata ? GetCryptoHandler() : nullptr;

  CPDF_FlateEncoder encoder(pStream->AsStream(), pStream != m_pMetadata);
  CPDF_Encryptor encryptor(pCrypto, objnum, encoder.GetSpan());
  if (static_cast<uint32_t>(encoder.GetDict()->GetIntegerFor("Length")) !=
      encryptor.GetSpan().size()) {
    encoder.CloneDict();
    encoder.GetClonedDict()->SetNewFor<CPDF_Number>(
        "Length", static_cast<int>(encryptor.GetSpan().size()));
  }

  if (!WriteDirectObj(objnum, encoder.GetDict(), true) ||
      !m_Archive->WriteString("stream\r\n")) {
    return false;
  }

  // Allow for empty streams.
  if (encryptor.GetSpan().size() > 0 &&
      !m_Archive->WriteBlock(encryptor.GetSpan().data(),
                             encryptor.GetSpan().size())) {
    return false;
  }

  return m_Archive->WriteString("\r\nendstream");
}

bool CPDF_Creator::WriteIndirectObj(uint32_t objnum, const CPDF_Object* pObj) {
  if (!m_Archive->WriteDWord(objnum) || !m_Archive->WriteString(" 0 obj\r\n"))
    return false;

  if (pObj->IsStream()) {
    if (!WriteStream(pObj, objnum))
      return false;
  } else if (!WriteDirectObj(objnum, pObj, true)) {
    return false;
  }

  return m_Archive->WriteString("\r\nendobj\r\n");
}

bool CPDF_Creator::WriteDirectObj(uint32_t objnum,
                                  const CPDF_Object* pObj,
                                  bool bEncrypt) {
  switch (pObj->GetType()) {
    case CPDF_Object::BOOLEAN:
    case CPDF_Object::NAME:
    case CPDF_Object::NULLOBJ:
    case CPDF_Object::NUMBER:
    case CPDF_Object::REFERENCE:
      if (!pObj->WriteTo(m_Archive.get()))
        return false;
      break;

    case CPDF_Object::STRING: {
      ByteString str = pObj->GetString();
      bool bHex = pObj->AsString()->IsHex();
      if (!GetCryptoHandler() || !bEncrypt) {
        if (!pObj->WriteTo(m_Archive.get()))
          return false;
        break;
      }
      CPDF_Encryptor encryptor(GetCryptoHandler(), objnum, str.AsRawSpan());
      ByteString content = PDF_EncodeString(
          ByteString(encryptor.GetSpan().data(), encryptor.GetSpan().size()),
          bHex);
      if (!m_Archive->WriteString(content.AsStringView()))
        return false;
      break;
    }
    case CPDF_Object::STREAM: {
      CPDF_FlateEncoder encoder(pObj->AsStream(), true);
      CPDF_Encryptor encryptor(GetCryptoHandler(), objnum, encoder.GetSpan());
      if (static_cast<uint32_t>(encoder.GetDict()->GetIntegerFor("Length")) !=
          encryptor.GetSpan().size()) {
        encoder.CloneDict();
        encoder.GetClonedDict()->SetNewFor<CPDF_Number>(
            "Length", static_cast<int>(encryptor.GetSpan().size()));
      }
      if (!WriteDirectObj(objnum, encoder.GetDict(), true) ||
          !m_Archive->WriteString("stream\r\n") ||
          !m_Archive->WriteBlock(encryptor.GetSpan().data(),
                                 encryptor.GetSpan().size()) ||
          !m_Archive->WriteString("\r\nendstream")) {
        return false;
      }

      break;
    }
    case CPDF_Object::ARRAY: {
      if (!m_Archive->WriteString("["))
        return false;

      const CPDF_Array* p = pObj->AsArray();
      for (size_t i = 0; i < p->GetCount(); i++) {
        if (!WriteDirectObj(objnum, p->GetObjectAt(i), true))
          return false;
      }
      if (!m_Archive->WriteString("]"))
        return false;
      break;
    }
    case CPDF_Object::DICTIONARY: {
      if (!GetCryptoHandler() || pObj == m_pEncryptDict) {
        if (!pObj->WriteTo(m_Archive.get()))
          return false;
        break;
      }

      if (!m_Archive->WriteString("<<"))
        return false;

      const CPDF_Dictionary* p = pObj->AsDictionary();
      bool bSignDict = p->IsSignatureDict();
      for (const auto& it : *p) {
        bool bSignValue = false;
        const ByteString& key = it.first;
        CPDF_Object* pValue = it.second.get();
        if (!m_Archive->WriteString("/") ||
            !m_Archive->WriteString(PDF_NameEncode(key).AsStringView())) {
          return false;
        }

        if (bSignDict && key == "Contents")
          bSignValue = true;
        if (!WriteDirectObj(objnum, pValue, !bSignValue))
          return false;
      }
      if (!m_Archive->WriteString(">>"))
        return false;
      break;
    }
  }
  return true;
}

bool CPDF_Creator::WriteOldIndirectObject(uint32_t objnum) {
  if (m_pParser->IsObjectFreeOrNull(objnum))
    return true;

  m_ObjectOffsets[objnum] = m_Archive->CurrentOffset();

  bool bExistInMap = !!m_pDocument->GetIndirectObject(objnum);
  CPDF_Object* pObj = m_pDocument->GetOrParseIndirectObject(objnum);
  if (!pObj) {
    m_ObjectOffsets.erase(objnum);
    return true;
  }
  if (!WriteIndirectObj(pObj->GetObjNum(), pObj))
    return false;
  if (!bExistInMap)
    m_pDocument->DeleteIndirectObject(objnum);
  return true;
}

bool CPDF_Creator::WriteOldObjs() {
  uint32_t nLastObjNum = m_pParser->GetLastObjNum();
  if (!m_pParser->IsValidObjectNumber(nLastObjNum))
    return true;

  for (uint32_t objnum = m_CurObjNum; objnum <= nLastObjNum; ++objnum) {
    if (!WriteOldIndirectObject(objnum))
      return false;
  }
  return true;
}

bool CPDF_Creator::WriteNewObjs() {
  for (size_t i = m_CurObjNum; i < m_NewObjNumArray.size(); ++i) {
    uint32_t objnum = m_NewObjNumArray[i];
    CPDF_Object* pObj = m_pDocument->GetIndirectObject(objnum);
    if (!pObj)
      continue;

    m_ObjectOffsets[objnum] = m_Archive->CurrentOffset();
    if (!WriteIndirectObj(pObj->GetObjNum(), pObj))
      return false;
  }
  return true;
}

void CPDF_Creator::InitNewObjNumOffsets() {
  for (const auto& pair : *m_pDocument) {
    const uint32_t objnum = pair.first;
    if (m_IsIncremental ||
        pair.second->GetObjNum() == CPDF_Object::kInvalidObjNum) {
      continue;
    }
    if (m_pParser && m_pParser->IsValidObjectNumber(objnum) &&
        !m_pParser->IsObjectFree(objnum)) {
      continue;
    }
    m_NewObjNumArray.insert(std::lower_bound(m_NewObjNumArray.begin(),
                                             m_NewObjNumArray.end(), objnum),
                            objnum);
  }
}

CPDF_Creator::Stage CPDF_Creator::WriteDoc_Stage1() {
  ASSERT(m_iStage > Stage::kInvalid || m_iStage < Stage::kInitWriteObjs20);
  if (m_iStage == Stage::kInit0) {
    if (!m_pParser || (m_bSecurityChanged && m_IsOriginal))
      m_IsIncremental = false;

    const CPDF_Dictionary* pDict = m_pDocument->GetRoot();
    m_pMetadata = pDict ? pDict->GetDirectObjectFor("Metadata") : nullptr;
    m_iStage = Stage::kWriteHeader10;
  }
  if (m_iStage == Stage::kWriteHeader10) {
    if (!m_IsIncremental) {
      if (!m_Archive->WriteString("%PDF-1."))
        return Stage::kInvalid;

      int32_t version = 7;
      if (m_FileVersion)
        version = m_FileVersion;
      else if (m_pParser)
        version = m_pParser->GetFileVersion();

      if (!m_Archive->WriteDWord(version % 10) ||
          !m_Archive->WriteString("\r\n%\xA1\xB3\xC5\xD7\r\n")) {
        return Stage::kInvalid;
      }
      m_iStage = Stage::kInitWriteObjs20;
    } else {
      m_SavedOffset = m_pParser->GetFileAccess()->GetSize();
      m_iStage = Stage::kWriteIncremental15;
    }
  }
  if (m_iStage == Stage::kWriteIncremental15) {
    if (m_IsOriginal && m_SavedOffset > 0) {
      RetainPtr<IFX_SeekableReadStream> pSrcFile = m_pParser->GetFileAccess();
      std::vector<uint8_t> buffer(4096);
      FX_FILESIZE src_size = m_SavedOffset;
      while (src_size) {
        uint32_t block_size = src_size > 4096 ? 4096 : src_size;
        if (!pSrcFile->ReadBlock(buffer.data(),
                                 m_Archive->CurrentOffset() - src_size,
                                 block_size)) {
          return Stage::kInvalid;
        }
        if (!m_Archive->WriteBlock(buffer.data(), block_size))
          return Stage::kInvalid;

        src_size -= block_size;
      }
    }
    if (m_IsOriginal && m_pParser->GetLastXRefOffset() == 0) {
      for (uint32_t num = 0; num <= m_pParser->GetLastObjNum(); ++num) {
        if (m_pParser->IsObjectFreeOrNull(num))
          continue;

        m_ObjectOffsets[num] = m_pParser->GetObjectPositionOrZero(num);
      }
    }
    m_iStage = Stage::kInitWriteObjs20;
  }
  InitNewObjNumOffsets();
  return m_iStage;
}

CPDF_Creator::Stage CPDF_Creator::WriteDoc_Stage2() {
  ASSERT(m_iStage >= Stage::kInitWriteObjs20 ||
         m_iStage < Stage::kInitWriteXRefs80);
  if (m_iStage == Stage::kInitWriteObjs20) {
    if (!m_IsIncremental && m_pParser) {
      m_CurObjNum = 0;
      m_iStage = Stage::kWriteOldObjs21;
    } else {
      m_iStage = Stage::kInitWriteNewObjs25;
    }
  }
  if (m_iStage == Stage::kWriteOldObjs21) {
    if (!WriteOldObjs())
      return Stage::kInvalid;

    m_iStage = Stage::kInitWriteNewObjs25;
  }
  if (m_iStage == Stage::kInitWriteNewObjs25) {
    m_CurObjNum = 0;
    m_iStage = Stage::kWriteNewObjs26;
  }
  if (m_iStage == Stage::kWriteNewObjs26) {
    if (!WriteNewObjs())
      return Stage::kInvalid;

    m_iStage = Stage::kWriteEncryptDict27;
  }
  if (m_iStage == Stage::kWriteEncryptDict27) {
    if (m_pEncryptDict && m_pEncryptDict->IsInline()) {
      m_dwLastObjNum += 1;
      FX_FILESIZE saveOffset = m_Archive->CurrentOffset();
      if (!WriteIndirectObj(m_dwLastObjNum, m_pEncryptDict.Get()))
        return Stage::kInvalid;

      m_ObjectOffsets[m_dwLastObjNum] = saveOffset;
      if (m_IsIncremental)
        m_NewObjNumArray.push_back(m_dwLastObjNum);
    }
    m_iStage = Stage::kInitWriteXRefs80;
  }
  return m_iStage;
}

CPDF_Creator::Stage CPDF_Creator::WriteDoc_Stage3() {
  ASSERT(m_iStage >= Stage::kInitWriteXRefs80 ||
         m_iStage < Stage::kWriteTrailerAndFinish90);

  uint32_t dwLastObjNum = m_dwLastObjNum;
  if (m_iStage == Stage::kInitWriteXRefs80) {
    m_XrefStart = m_Archive->CurrentOffset();
    if (!m_IsIncremental || !m_pParser->IsXRefStream()) {
      if (!m_IsIncremental || m_pParser->GetLastXRefOffset() == 0) {
        ByteString str;
        str = pdfium::ContainsKey(m_ObjectOffsets, 1)
                  ? "xref\r\n"
                  : "xref\r\n0 1\r\n0000000000 65535 f\r\n";
        if (!m_Archive->WriteString(str.AsStringView()))
          return Stage::kInvalid;

        m_CurObjNum = 1;
        m_iStage = Stage::kWriteXrefsNotIncremental81;
      } else {
        if (!m_Archive->WriteString("xref\r\n"))
          return Stage::kInvalid;

        m_CurObjNum = 0;
        m_iStage = Stage::kWriteXrefsIncremental82;
      }
    } else {
      m_iStage = Stage::kWriteTrailerAndFinish90;
    }
  }
  if (m_iStage == Stage::kWriteXrefsNotIncremental81) {
    ByteString str;
    uint32_t i = m_CurObjNum;
    uint32_t j;
    while (i <= dwLastObjNum) {
      while (i <= dwLastObjNum && !pdfium::ContainsKey(m_ObjectOffsets, i))
        i++;

      if (i > dwLastObjNum)
        break;

      j = i;
      while (j <= dwLastObjNum && pdfium::ContainsKey(m_ObjectOffsets, j))
        j++;

      if (i == 1)
        str = ByteString::Format("0 %d\r\n0000000000 65535 f\r\n", j);
      else
        str = ByteString::Format("%d %d\r\n", i, j - i);

      if (!m_Archive->WriteString(str.AsStringView()))
        return Stage::kInvalid;

      while (i < j) {
        str = ByteString::Format("%010d 00000 n\r\n", m_ObjectOffsets[i++]);
        if (!m_Archive->WriteString(str.AsStringView()))
          return Stage::kInvalid;
      }
      if (i > dwLastObjNum)
        break;
    }
    m_iStage = Stage::kWriteTrailerAndFinish90;
  }
  if (m_iStage == Stage::kWriteXrefsIncremental82) {
    ByteString str;
    uint32_t iCount = pdfium::CollectionSize<uint32_t>(m_NewObjNumArray);
    uint32_t i = m_CurObjNum;
    while (i < iCount) {
      size_t j = i;
      uint32_t objnum = m_NewObjNumArray[i];
      while (j < iCount) {
        if (++j == iCount)
          break;
        uint32_t dwCurrent = m_NewObjNumArray[j];
        if (dwCurrent - objnum > 1)
          break;
        objnum = dwCurrent;
      }
      objnum = m_NewObjNumArray[i];
      if (objnum == 1)
        str = ByteString::Format("0 %d\r\n0000000000 65535 f\r\n", j - i + 1);
      else
        str = ByteString::Format("%d %d\r\n", objnum, j - i);

      if (!m_Archive->WriteString(str.AsStringView()))
        return Stage::kInvalid;

      while (i < j) {
        objnum = m_NewObjNumArray[i++];
        str = ByteString::Format("%010d 00000 n\r\n", m_ObjectOffsets[objnum]);
        if (!m_Archive->WriteString(str.AsStringView()))
          return Stage::kInvalid;
      }
    }
    m_iStage = Stage::kWriteTrailerAndFinish90;
  }
  return m_iStage;
}

CPDF_Creator::Stage CPDF_Creator::WriteDoc_Stage4() {
  ASSERT(m_iStage >= Stage::kWriteTrailerAndFinish90);

  bool bXRefStream = m_IsIncremental && m_pParser->IsXRefStream();
  if (!bXRefStream) {
    if (!m_Archive->WriteString("trailer\r\n<<"))
      return Stage::kInvalid;
  } else {
    if (!m_Archive->WriteDWord(m_pDocument->GetLastObjNum() + 1) ||
        !m_Archive->WriteString(" 0 obj <<")) {
      return Stage::kInvalid;
    }
  }

  if (m_pParser) {
    std::unique_ptr<CPDF_Dictionary> p = m_pParser->GetCombinedTrailer();
    for (const auto& it : *p) {
      const ByteString& key = it.first;
      CPDF_Object* pValue = it.second.get();
      if (key == "Encrypt" || key == "Size" || key == "Filter" ||
          key == "Index" || key == "Length" || key == "Prev" || key == "W" ||
          key == "XRefStm" || key == "ID" || key == "DecodeParms" ||
          key == "Type") {
        continue;
      }
      if (!m_Archive->WriteString(("/")) ||
          !m_Archive->WriteString(PDF_NameEncode(key).AsStringView())) {
        return Stage::kInvalid;
      }
      if (!pValue->WriteTo(m_Archive.get()))
        return Stage::kInvalid;
    }
  } else {
    if (!m_Archive->WriteString("\r\n/Root ") ||
        !m_Archive->WriteDWord(m_pDocument->GetRoot()->GetObjNum()) ||
        !m_Archive->WriteString(" 0 R\r\n")) {
      return Stage::kInvalid;
    }
    if (m_pDocument->GetInfo()) {
      if (!m_Archive->WriteString("/Info ") ||
          !m_Archive->WriteDWord(m_pDocument->GetInfo()->GetObjNum()) ||
          !m_Archive->WriteString(" 0 R\r\n")) {
        return Stage::kInvalid;
      }
    }
  }
  if (m_pEncryptDict) {
    if (!m_Archive->WriteString("/Encrypt"))
      return Stage::kInvalid;

    uint32_t dwObjNum = m_pEncryptDict->GetObjNum();
    if (dwObjNum == 0)
      dwObjNum = m_pDocument->GetLastObjNum() + 1;
    if (!m_Archive->WriteString(" ") || !m_Archive->WriteDWord(dwObjNum) ||
        !m_Archive->WriteString(" 0 R ")) {
      return Stage::kInvalid;
    }
  }

  if (!m_Archive->WriteString("/Size ") ||
      !m_Archive->WriteDWord(m_dwLastObjNum + (bXRefStream ? 2 : 1))) {
    return Stage::kInvalid;
  }
  if (m_IsIncremental) {
    FX_FILESIZE prev = m_pParser->GetLastXRefOffset();
    if (prev) {
      if (!m_Archive->WriteString("/Prev "))
        return Stage::kInvalid;

      char offset_buf[20];
      memset(offset_buf, 0, sizeof(offset_buf));
      FXSYS_i64toa(prev, offset_buf, 10);
      if (!m_Archive->WriteBlock(offset_buf, strlen(offset_buf)))
        return Stage::kInvalid;
    }
  }
  if (m_pIDArray) {
    if (!m_Archive->WriteString(("/ID")) ||
        !m_pIDArray->WriteTo(m_Archive.get())) {
      return Stage::kInvalid;
    }
  }
  if (!bXRefStream) {
    if (!m_Archive->WriteString(">>"))
      return Stage::kInvalid;
  } else {
    if (!m_Archive->WriteString("/W[0 4 1]/Index["))
      return Stage::kInvalid;
    if (m_IsIncremental && m_pParser && m_pParser->GetLastXRefOffset() == 0) {
      uint32_t i = 0;
      for (i = 0; i < m_dwLastObjNum; i++) {
        if (!pdfium::ContainsKey(m_ObjectOffsets, i))
          continue;
        if (!m_Archive->WriteDWord(i) || !m_Archive->WriteString(" 1 "))
          return Stage::kInvalid;
      }
      if (!m_Archive->WriteString("]/Length ") ||
          !m_Archive->WriteDWord(m_dwLastObjNum * 5) ||
          !m_Archive->WriteString(">>stream\r\n")) {
        return Stage::kInvalid;
      }
      for (i = 0; i < m_dwLastObjNum; i++) {
        auto it = m_ObjectOffsets.find(i);
        if (it == m_ObjectOffsets.end())
          continue;
        OutputIndex(m_Archive.get(), it->second);
      }
    } else {
      size_t count = m_NewObjNumArray.size();
      size_t i = 0;
      for (i = 0; i < count; i++) {
        if (!m_Archive->WriteDWord(m_NewObjNumArray[i]) ||
            !m_Archive->WriteString(" 1 ")) {
          return Stage::kInvalid;
        }
      }
      if (!m_Archive->WriteString("]/Length ") ||
          !m_Archive->WriteDWord(count * 5) ||
          !m_Archive->WriteString(">>stream\r\n")) {
        return Stage::kInvalid;
      }
      for (i = 0; i < count; ++i)
        OutputIndex(m_Archive.get(), m_ObjectOffsets[m_NewObjNumArray[i]]);
    }
    if (!m_Archive->WriteString("\r\nendstream"))
      return Stage::kInvalid;
  }

  if (!m_Archive->WriteString("\r\nstartxref\r\n"))
    return Stage::kInvalid;

  char offset_buf[20];
  memset(offset_buf, 0, sizeof(offset_buf));
  FXSYS_i64toa(m_XrefStart, offset_buf, 10);
  if (!m_Archive->WriteBlock(offset_buf, strlen(offset_buf)) ||
      !m_Archive->WriteString("\r\n%%EOF\r\n")) {
    return Stage::kInvalid;
  }

  m_iStage = Stage::kComplete100;
  return m_iStage;
}

bool CPDF_Creator::Create(uint32_t flags) {
  m_IsIncremental = !!(flags & FPDFCREATE_INCREMENTAL);
  m_IsOriginal = !(flags & FPDFCREATE_NO_ORIGINAL);

  m_iStage = Stage::kInit0;
  m_dwLastObjNum = m_pDocument->GetLastObjNum();
  m_ObjectOffsets.clear();
  m_NewObjNumArray.clear();

  InitID();
  return Continue();
}

void CPDF_Creator::InitID() {
  ASSERT(!m_pIDArray);

  m_pIDArray = pdfium::MakeUnique<CPDF_Array>();
  const CPDF_Array* pOldIDArray = m_pParser ? m_pParser->GetIDArray() : nullptr;
  const CPDF_Object* pID1 = pOldIDArray ? pOldIDArray->GetObjectAt(0) : nullptr;
  if (pID1) {
    m_pIDArray->Add(pID1->Clone());
  } else {
    std::vector<uint8_t> buffer =
        GenerateFileID((uint32_t)(uintptr_t)this, m_dwLastObjNum);
    ByteString bsBuffer(buffer.data(), buffer.size());
    m_pIDArray->AddNew<CPDF_String>(bsBuffer, true);
  }

  if (pOldIDArray) {
    const CPDF_Object* pID2 = pOldIDArray->GetObjectAt(1);
    if (m_IsIncremental && m_pEncryptDict && pID2) {
      m_pIDArray->Add(pID2->Clone());
      return;
    }
    std::vector<uint8_t> buffer =
        GenerateFileID((uint32_t)(uintptr_t)this, m_dwLastObjNum);
    ByteString bsBuffer(buffer.data(), buffer.size());
    m_pIDArray->AddNew<CPDF_String>(bsBuffer, true);
    return;
  }

  m_pIDArray->Add(m_pIDArray->GetObjectAt(0)->Clone());
  if (m_pEncryptDict) {
    ASSERT(m_pParser);
    if (m_pEncryptDict->GetStringFor("Filter") == "Standard") {
      ByteString user_pass = m_pParser->GetPassword();
      m_pSecurityHandler = pdfium::MakeUnique<CPDF_SecurityHandler>();
      m_pSecurityHandler->OnCreate(m_pEncryptDict.Get(), m_pIDArray.get(),
                                   user_pass);
      m_bSecurityChanged = true;
    }
  }
}

bool CPDF_Creator::Continue() {
  if (m_iStage < Stage::kInit0)
    return false;

  Stage iRet = Stage::kInit0;
  while (m_iStage < Stage::kComplete100) {
    if (m_iStage < Stage::kInitWriteObjs20)
      iRet = WriteDoc_Stage1();
    else if (m_iStage < Stage::kInitWriteXRefs80)
      iRet = WriteDoc_Stage2();
    else if (m_iStage < Stage::kWriteTrailerAndFinish90)
      iRet = WriteDoc_Stage3();
    else
      iRet = WriteDoc_Stage4();

    if (iRet < m_iStage)
      break;
  }

  if (iRet <= Stage::kInit0 || m_iStage == Stage::kComplete100) {
    m_iStage = Stage::kInvalid;
    return iRet > Stage::kInit0;
  }

  return m_iStage > Stage::kInvalid;
}

bool CPDF_Creator::SetFileVersion(int32_t fileVersion) {
  if (fileVersion < 10 || fileVersion > 17)
    return false;
  m_FileVersion = fileVersion;
  return true;
}

void CPDF_Creator::RemoveSecurity() {
  m_pSecurityHandler.Reset();
  m_bSecurityChanged = true;
  m_pEncryptDict = nullptr;
}

CPDF_CryptoHandler* CPDF_Creator::GetCryptoHandler() {
  return m_pSecurityHandler ? m_pSecurityHandler->GetCryptoHandler() : nullptr;
}
