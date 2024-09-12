### Headers folder (./source/include)
Header files are saved within ./source/include

### Pragma once
Headers should use `#pragma once` directive. It's non-standard but widely supported preprocessor directive in C and C++. It's used as an include guard to prevent multiple inclusions of the same header file.

### UpperCamelCase file names
Application.cpp / class Application()

* Often preferred in Unix-like environments and by many open-source projects.
* Adheres to the convention that most filenames are lowercase, which can be easier to type and less error-prone in case-sensitive file systems
