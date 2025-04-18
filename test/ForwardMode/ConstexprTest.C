// RUN: %cladclang %s -I%S/../../include -std=c++23 -oConstexprTest.out | %filecheck %s
// RUN: ./ConstexprTest.out | %filecheck_exec %s
// UNSUPPORTED: clang-8, clang-9, clang-10, clang-11, clang-12, clang-13, clang-14, clang-15, clang-16

#include "clad/Differentiator/Differentiator.h"

constexpr double fn(double x, double y) {
    return (x + y) / 2;
}

//CHECK: constexpr double fn_darg0(double x, double y) {
//CHECK-NEXT:    double _d_x = 1;
//CHECK-NEXT:    double _d_y = 0;
//CHECK-NEXT:    double _t0 = (x + y);
//CHECK-NEXT:    return ((_d_x + _d_y) * 2 - _t0 * 0) / (2 * 2);
//CHECK-NEXT:}

constexpr double mul(double a, double b, double c) {
     double val = 99.00;
     double result = val * a + 100 - b + c;
     return result;
}

//CHECK: constexpr double mul_darg0(double a, double b, double c) {
//CHECK-NEXT:    double _d_a = 1;
//CHECK-NEXT:    double _d_b = 0;
//CHECK-NEXT:    double _d_c = 0;
//CHECK-NEXT:    double _d_val = 0.;
//CHECK-NEXT:    double val = 99.;
//CHECK-NEXT:    double _d_result = _d_val * a + val * _d_a + 0 - _d_b + _d_c;
//CHECK-NEXT:    double result = val * a + 100 - b + c;
//CHECK-NEXT:    return _d_result;
//CHECK-NEXT:}

constexpr double fn_test() {
    if consteval {
	auto dx = clad::differentiate<clad::immediate_mode>(fn, "x");

	return dx.execute(4, 7);
    } else {
	assert(false && "fn non-immediate context");
	return -1.;
    }
}

constexpr double mul_test() {
    if consteval {
	auto dx = clad::differentiate<clad::immediate_mode>(mul, "a");

	return dx.execute(5, 6, 10);
    } else {
	assert(false && "mul non-immediate context");
	return -1.;
    }
}

int main() {
    constexpr double fn_result = fn_test();
    printf("%.2f\n", fn_result); // CHECK-EXEC: 0.50

    constexpr double mul_result = mul_test();
    printf("%.2f\n", mul_result); // CHECK-EXEC: 99.0
}
