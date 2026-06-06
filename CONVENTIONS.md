### Headers folder (./source/include)
Header-only headers in include/ are globally available and source/*.cpp is globbed automatically.

### Pragma once
Headers should use `#pragma once` directive to guard to prevent multiple inclusions of the same header file.

### Lower Camel Case file names
application.cpp / class Application()