#include <math.h>

/* Preserve small arguments when the NuttX libm lacks log1p. */

double log1p(double x)
{
  double sum = 1.0 + x;
  if (sum == 1.0) return x;
  if (sum <= 0.0 || !isfinite(sum)) return log(sum);
  return log(sum) * x / (sum - 1.0);
}
