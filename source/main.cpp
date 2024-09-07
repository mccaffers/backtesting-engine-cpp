#include <iostream>
#include "main.hpp"

int main(int argc, const char * argv[]) {
  // insert code here...
  std::cout << "Hello, World!\n";
  std::vector<int> numbers = {-1, -2, 3, 4, -5};
  std::cout << Main::addNumbers(numbers) << std::endl;
  return 0;
}
