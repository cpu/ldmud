#pragma weak_types

// LPC allows a function to be called before it's defined,
// but only with with type checks disabled.

run_test()
{
    int fun = 10;

    return fun() == 42;
}

fun()
{
    return 42;
}
