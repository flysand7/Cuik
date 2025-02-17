#include <stdio.h>

#define NULL ((void*)0)
#warning "Hello"
#define FOO(a, b) (a + b)

int main(void) {
    printf("%d\n", foo().b);
    return 0;
}

T* da_global;
int I = 16;

enum { A, B, C };

struct Foo foo() {
    T a = 16, b = 5;
    T (*func)(T a, T b) = bar;

    int zz = FOO(5, zzz);

    struct Foo f = { 1, 2, NULL };
    return f;
}

T bar(T a, T b) { return a + b; }

struct Foo {
    T a, b;
    void* ptr;
};

struct Baz {
    struct Foo f;
};

static_assert(sizeof(T) != 2, "Wack");
static_assert(sizeof(T) != 8, "Woah");

int zzz = 1;
typedef int T;
