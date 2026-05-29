class CalPoint:
    def __init__(self, dac_code, measured_v):
        self.dac_code = dac_code
        self.measured_v = measured_v
    
    def __repr__(self):
        return f"({self.dac_code}, {self.measured_v:.3f}V)"

# Let's generate a list of points from -116 to 126 with step 2
# We know:
# dac=-116 -> v=35.291
# dac=-114 -> v=34.983
# dac=-112 -> v=34.705
# Let's fit a line or use the actual steps:
# step from -116 to -114 is (34.983 - 35.291)/2 = -0.154 V/dac_code
# step from -114 to -112 is (34.705 - 34.983)/2 = -0.139 V/dac_code
# Let's assume an average step of -0.147 V/dac_code.
# Let's build the points list:
points = []
v = 35.291
c = -116
points.append(CalPoint(c, v))
while c < 126:
    c += 2
    if c <= -112:
        if c == -114:
            v = 34.983
        else:
            v = 34.705
    else:
        v -= 2 * 0.147
    points.append(CalPoint(c, v))

def voltage_to_code_sim(points, volts):
    count = len(points)
    for i in range(count - 1):
        v0 = points[i].measured_v
        v1 = points[i+1].measured_v
        c0 = points[i].dac_code
        c1 = points[i+1].dac_code
        
        between = (v0 <= volts <= v1) or (v1 <= volts <= v0)
        if between and v0 != v1:
            t = (volts - v0) / (v1 - v0)
            code_f = float(c0) + t * float(c1 - c0)
            
            # C implementation:
            # int code_i = (code_f >= 0) ? (int)floorf(code_f) : (int)ceilf(code_f);
            import math
            if code_f >= 0:
                code_i = int(math.floor(code_f))
            else:
                code_i = int(math.ceil(code_f))
            print(f"Matched between indices {i} and {i+1}:")
            print(f"  v0={v0:.3f}, v1={v1:.3f}, c0={c0}, c1={c1}")
            print(f"  volts={volts:.3f}, t={t:.6f}, code_f={code_f:.6f}, code_i={code_i}")
            return code_i
            
    # Best match fallback
    best_idx = 0
    best_err = abs(points[0].measured_v - volts)
    for i in range(1, count):
        err = abs(points[i].measured_v - volts)
        if err < best_err:
            best_err = err
            best_idx = i
    print(f"Matched nearest: idx={best_idx}, code={points[best_idx].dac_code}, err={best_err:.3f}")
    return points[best_idx].dac_code

target = 27.10
code = voltage_to_code_sim(points, target)
# Now let's see what voltage that code actually outputs under the same model:
v_actual = 35.291 - (code - (-116)) * 0.147
print(f"Estimated voltage at code {code}: {v_actual:.3f}V")
