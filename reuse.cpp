#include <iostream>

int main() {
    for (int i = 0; i < 10; ++i) {
        int* p = new int(i);
        std::cout << p << '\n';
        delete p;
    }
}