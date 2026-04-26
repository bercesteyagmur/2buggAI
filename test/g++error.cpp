#include <iostream>
#include <functional>
#include <memory>
using namespace std;

int main() {
    unique_ptr<int> ptr(new int(42));
    // Bug: In einigen älteren g++-Versionen wurde das direkte Verschieben 
    // von move-only Objekten im Lambda-Capture fehlerhaft behandelt.
    function<void()> f = [p = move(ptr)]() {
        cout << *p << endl;
    };
    f();
    return 0;
}