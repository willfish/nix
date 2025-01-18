#define MAX_RAY_MARCH_STEPS 32
#define MAX_DISTANCE 4.0
#define SURFACE_DISTANCE 0.002

struct Hit
{
    float dist;
    float closest_dist;
    vec3 p;
};

float specularBlinnPhong(vec3 light_dir, vec3 ray_dir, vec3 normal)
{
    vec3 halfway = normalize(light_dir + ray_dir);
    return max(0.0, dot(normal, halfway));
}

vec4 mod289(vec4 x){return x - floor(x * (1.0 / 289.0)) * 289.0;}
vec4 perm(vec4 x){return mod289(((x * 34.0) + 1.0) * x);}

float noise(vec3 p)
{
    vec3 a = floor(p);
    vec3 d = p - a;
    d = d * d * (3.0 - 2.0 * d);

    vec4 b = a.xxyy + vec4(0.0, 1.0, 0.0, 1.0);
    vec4 k1 = perm(b.xyxy);
    vec4 k2 = perm(k1.xyxy + b.zzww);

    vec4 c = k2 + a.zzzz;
    vec4 k3 = perm(c);
    vec4 k4 = perm(c + 1.0);

    vec4 o1 = fract(k3 * (1.0 / 41.0));
    vec4 o2 = fract(k4 * (1.0 / 41.0));

    vec4 o3 = o2 * d.z + o1 * (1.0 - d.z);
    vec2 o4 = o3.yw * d.x + o3.xz * (1.0 - d.x);

    return o4.y * d.y + o4.x * (1.0 - d.y);
}

float SDF(vec3 point)
{
    vec3 p = vec3(point.xy, iTime * 0.3 + point.z);
    float n = (noise(p) + noise(p * 2.0) * 0.5 + noise(p * 4.0) * 0.25) * 0.57;
    return length(point) - 0.35 - n * 0.3;
}

vec3 getNormal(vec3 point)
{
    vec2 e = vec2(0.002, 0.0);
    return normalize(SDF(point) - vec3(SDF(point - e.xyy), SDF(point - e.yxy), SDF(point - e.yyx)));
}

Hit raymarch(vec3 p, vec3 d)
{
    Hit hit;
    hit.closest_dist = MAX_DISTANCE;
    for (int i = 0; i < MAX_RAY_MARCH_STEPS; ++i)
    {
        float sdf = SDF(p);
        p += d * sdf;
        hit.closest_dist = min(hit.closest_dist, sdf);
        hit.dist += sdf;
        if (hit.dist >= MAX_DISTANCE || abs(sdf) <= SURFACE_DISTANCE)
            break;
    }

    hit.p = p;
    return hit;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (fragCoord * 2.0 - iResolution.xy) / iResolution.y;
    fragColor = vec4(0, 0, 0, 1);
    if (dot(uv, uv) > 1.0) return;
    vec3 pos = vec3(0, 0, -1);
    vec3 dir = normalize(vec3(uv, 1));

    Hit hit = raymarch(pos, dir);
    fragColor = vec4(pow(max(0.0, 1.0 - hit.closest_dist), 32.0) * (max(0.0, dot(uv, vec2(0.707))) * vec3(0.3, 0.65, 1.0) + max(0.0, dot(uv, vec2(-0.707))) * vec3(0.6, 0.35, 1.0) + vec3(0.4, 0.5, 1.0)), max(0.0, hit.closest_dist));
    if (hit.closest_dist >= SURFACE_DISTANCE)
        return;
    vec3 normal = getNormal(hit.p);

    vec3 ray_dir = normalize(pos - hit.p);
    float facing = max(0.0, sqrt(dot(normal, vec3(0.707, 0.707, 0))) * 1.5 - dot(normal, -dir));
    fragColor = mix(vec4(0), vec4(0.3, 0.65, 1.0, 1.0), 0.75 * facing * facing * facing);

    facing = max(0.0, sqrt(dot(normal, vec3(-0.707, -0.707, 0))) * 1.5 - dot(normal, -dir));
    fragColor = vec4(fragColor.rgb, 0) + mix(vec4(0), vec4(0.6, 0.35, 1.0, 1.0), 0.75 * facing * facing * facing);

    facing = max(0.0, sqrt(dot(normal, vec3(0.0, 0.0, -1.0))) * 1.5 - dot(normal, -dir));
    fragColor = vec4(fragColor.rgb, 0) + mix(vec4(0), vec4(0.4, 0.5, 1.0, 1.0), 0.5 * facing * facing * facing);

    fragColor = vec4(fragColor.rgb, 0) + mix(vec4(0), vec4(0.4, 0.625, 1.0, 1.0), pow(specularBlinnPhong(normalize(vec3(600, 800, -500) - hit.p), ray_dir, normal), 12.0) * 1.0);
    fragColor = vec4(fragColor.rgb, 0) + mix(vec4(0), vec4(0.6, 0.5625, 1.0, 1.0), pow(specularBlinnPhong(normalize(vec3(-600, -800, -00) - hit.p), ray_dir, normal), 16.0) * 0.75);
    fragColor.rgb = pow(fragColor.rgb, vec3(1.25));
}
