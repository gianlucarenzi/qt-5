// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_BASE_MOJO_CLIPBOARD_BLINK_STRUCT_TRAITS_H_
#define UI_BASE_MOJO_CLIPBOARD_BLINK_STRUCT_TRAITS_H_

#include "third_party/blink/public/mojom/clipboard/clipboard.mojom-shared.h"
#include "ui/base/clipboard/clipboard_types.h"

namespace mojo {

template <>
struct EnumTraits<blink::mojom::ClipboardBuffer, ui::ClipboardType> {
  static blink::mojom::ClipboardBuffer ToMojom(
      ui::ClipboardType clipboard_type) {
    // We only convert ui::Clipboardtype to blink::mojom::ClipboardBuffer
    // in tests, and they use ui::CLIPBOARD_TYPE_COPY_PASTE.
    DCHECK(clipboard_type == ui::CLIPBOARD_TYPE_COPY_PASTE);
    return blink::mojom::ClipboardBuffer::kStandard;
  }

  static bool FromMojom(blink::mojom::ClipboardBuffer clipboard_type,
                        ui::ClipboardType* out) {
    switch (clipboard_type) {
      case blink::mojom::ClipboardBuffer::kStandard:
        *out = ui::CLIPBOARD_TYPE_COPY_PASTE;
        return true;
      case blink::mojom::ClipboardBuffer::kSelection:
        // mirroring ui/base/clipboard/clipboard.h
#if !defined(OS_WIN) && !defined(OS_MACOSX) && !defined(OS_CHROMEOS)
        *out = ui::CLIPBOARD_TYPE_SELECTION;
        return true;
#else
        return false;
#endif
    }
    return false;
  }
};

}  // namespace mojo

#endif  // UI_BASE_MOJO_CLIPBOARD_BLINK_STRUCT_TRAITS_H_
