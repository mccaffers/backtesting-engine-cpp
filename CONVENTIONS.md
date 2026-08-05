### Source layout & includes
Plain headers live next to the code they belong to under `source/` (e.g.
`source/shared/utilities/`, `source/shared/tradingDefinitions/`) — there is no
separate `include/` folder. The build exposes a single source root (`source/`),
so every project `#include` is **source-relative and path-qualified**, e.g.
`#include "shared/utilities/env.hpp"`. This makes each include / module global
module fragment dependency self-describing. (The build also exposes a second
PUBLIC include root, `external/`, only for vendored third-party headers like
`nlohmann/json.hpp`.) Both `source/*.cpp` and `source/*.cppm` are globbed
automatically (`CONFIGURE_DEPENDS`), so a new file under `source/` is picked up
without editing `CMakeLists.txt` — `source/` only; tests are not globbed, see
below.

### Pragma once
Headers should use `#pragma once` directive to guard to prevent multiple inclusions of the same header file.

### Naming
File names are lowerCamelCase, types are PascalCase, namespaces are snake_case.
databaseConnection.cppm / class DatabaseConnection / namespace symbol_scale
(the one legacy camelCase namespace is tradingDefinitions)

### New files should come with ctests
only exception would be if they are using libraries (eg. boost), we don't need to test libraries.
Unlike `source/`, test translation units are NOT globbed: `tests/CMakeLists.txt`
lists every file explicitly in `add_executable(unit_tests ...)`, so a new
`tests/foo.cpp` silently never builds or runs until added there. Tests register
with ctest via `catch_discover_tests` in the same file.