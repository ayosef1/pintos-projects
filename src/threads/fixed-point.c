#include "threads/fixed-point.h"

/* Converts integer to fixed point representation */
fixed_point 
int_to_fp (int n) 
{
    return n * F;
}

/* Converts fixed point representation to integer.
   Rounds toward nearest integer. */
int 
fp_to_int (fixed_point fp) 
{
    if (fp > 0) return (fp + F / 2) / F;
    return (fp - F / 2) / F;
}

/* Sums two fixed points. */
fixed_point 
fp_add (fixed_point x, fixed_point y) 
{
    return x + y;
}

/* Subtracts fixed point y from fixed point x. */
fixed_point 
fp_sub (fixed_point x, fixed_point y) 
{
    return x - y;
}

/* Multiplies two fixed points. */
fixed_point 
fp_mult (fixed_point x, fixed_point y) 
{
    return ((int64_t) x) * y / F;
}

/* Divides fixed point x by y. */
fixed_point 
fp_div (fixed_point x, fixed_point y) 
{
    return ((int64_t) x) * F / y;
}

/* Adds int to fixed point and returns sum as fixed point. */
fixed_point 
add_int_to_fp (fixed_point fp, int n) 
{
    return fp + n * F;
}

/* Subtracts int from fixed point and returns difference as fixed point. */
fixed_point 
sub_int_from_fp (fixed_point fp, int n) 
{
    return fp - n * F;
}

/* Multiplies fixed point by int and returns product as fixed point. */
fixed_point 
mult_fp_by_int (fixed_point fp, int n) 
{
    return fp * n;
}

/* Divides fixed point by int and returns quotient as fixed point. */
fixed_point 
div_fp_by_int (fixed_point fp, int n) 
{
    return fp / n;
}