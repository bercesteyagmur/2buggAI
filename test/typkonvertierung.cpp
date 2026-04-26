#include <iostream>
using namespace std;

class Base {
public:
    virtual void foo() { cout << "Base::foo" << endl; }
};

class Derived : public Base {
public:
    void foo() override { cout << "Derived::foo" << endl; }
};

int main() {
    Base* b = new Base();
    // Fehlerhafte Typkonvertierung: Es wird mittels reinterpret_cast ein Zeiger
    // in einen anderen Typ umgewandelt, ohne dass die tatsächliche Objektart
    // übereinstimmt.
    Derived* d = reinterpret_cast<Derived*>(b);
    // Der Aufruf von d->foo() führt zu undefiniertem Verhalten, da b kein Derived-Objekt ist.
    d->foo();
    delete b;
    return 0;
}