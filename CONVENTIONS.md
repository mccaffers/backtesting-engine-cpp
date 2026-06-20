### Source layout & includes
Plain headers live next to the code they belong to under `source/` (e.g.
`source/shared/utilities/`, `source/shared/tradingDefinitions/`) — there is no
separate `include/` folder. The build exposes a single source root (`source/`),
so every project `#include` is **source-relative and path-qualified**, e.g.
`#include "shared/utilities/env.hpp"`. This makes each include / module global
module fragment dependency self-describing. Both `source/*.cpp` and
`source/*.cppm` are globbed automatically (`CONFIGURE_DEPENDS`), so a new file is
picked up without editing `CMakeLists.txt`.

### Pragma once
Headers should use `#pragma once` directive to guard to prevent multiple inclusions of the same header file.

### Lower Camel Case names
camelCase applies to file names, types, and namespaces.
application.cpp / class Application() / namespace tradingDefinitions
