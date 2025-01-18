#define TAU 6.28318530718

#define SPEED 0.33
#define INK_COLOR vec3(0, 0, 0)
#define BACKGROUND_COLOR vec3(1)
#define POSITION vec2(0)

float rand(vec2 n) {
	return fract(sin(dot(n, vec2(12.9898, 4.1414))) * 43758.5453);
}

float noise(vec2 p){
	vec2 ip = floor(p);
	vec2 u = fract(p);
	u = u*u*(3.0-2.0*u);

	float res = mix(
		mix(rand(ip),rand(ip+vec2(1.0,0.0)),u.x),
		mix(rand(ip+vec2(0.0,1.0)),rand(ip+vec2(1.0,1.0)),u.x),u.y);
	return res*res;
}

float fbm(vec2 p, int octaves)
{
    float n = 0.0;
    float a = 1.0;
    float norm = 0.0;
    for(int i = 0; i < octaves; ++i)
    {
        n += noise(p) * a;
        norm += a;
        p *= 2.0;
        a *= 0.5;
    }
    return n / norm;
}

void mainImage( out vec4 fragColor, in vec2 fragCoord )
{
    float time = mod(iTime, 2.5 / SPEED);
    vec2 uv = (fragCoord * 2.0 - iResolution.xy - POSITION * iResolution.xy) / iResolution.y;

    float angle = atan(uv.y, uv.x);
    angle += fbm(uv * 4.0, 2) * 0.5;
    vec2 p = vec2(cos(angle), sin(angle));

    float t = time * SPEED;
    t *= t;

    float l = dot(uv / t, uv / t);
    l -= (fbm(normalize(uv) * 3.0, 2) - 0.5);
    float ink = fbm(p * 8.0, 2) + 1.5 - l;

    vec3 col = mix(BACKGROUND_COLOR, INK_COLOR, clamp(ink, 0.0, 1.0));

    fragColor = vec4(col, 1.0);
}
