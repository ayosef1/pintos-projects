#include <stdint.h>

/* We use a signed 32-bit integer to create
   a p.q fixed point number representation. */
typedef int32_t fixed_point;

/* Default to using 17.14 representation. 
   Note: p + q must equal 31 */
#define P 17
#define Q 14
#define F (1 << Q)

fixed_point int_to_fp (int n);

int fp_to_int (fixed_point fp);

fixed_point fp_add (fixed_point x, fixed_point y);

fixed_point fp_sub (fixed_point x, fixed_point y);

fixed_point fp_mult (fixed_point x, fixed_point y);

fixed_point fp_div (fixed_point x, fixed_point y);

fixed_point add_int_to_fp (fixed_point fp, int n);

fixed_point sub_int_from_fp (fixed_point fp, int n);

fixed_point mult_fp_by_int (fixed_point fp, int n);

fixed_point div_fp_by_int (fixed_point fp, int n);