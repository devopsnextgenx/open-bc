// viewer_kind.h
// ---------------------------------------------------------------------------
// The one enum that names "which compare viewer a file belongs to". Kept free
// of Qt on purpose so the drop router (drop_router.h) and its standalone unit
// test (tools/drop_router_test) can use it without linking Qt. file_kinds.h
// includes this and keeps owning the extension tables + classifyFilePath().
// ---------------------------------------------------------------------------
#pragma once

namespace openbc::app {

enum class ViewerKind { Text, Picture, Model3d, Table, Media, Registry, Version, Hex };

}  // namespace openbc::app
