#include <iostream>

void crashFunction() {
    int* ptr = nullptr;
    *ptr = 10;  // Crash hier
}

int main() {
    std::cout << "Program started" << std::endl;
    crashFunction();
    std::cout << "Program ended" << std::endl;
    return 0;
}
