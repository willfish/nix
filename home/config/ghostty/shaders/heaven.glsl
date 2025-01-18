// Original shader by Yohei Nishitsuji (https://x.com/YoheiNishitsuji/status/1880399305196073072)
// Adapted for Shadertoy

// HSV to RGB color conversion
vec3 hsv(float h,float s,float v){
    vec4 t=vec4(1.,2./3.,1./3.,3.);
    vec3 p=abs(fract(vec3(h)+t.xyz)*6.-vec3(t.w));
    return v*mix(vec3(t.x),clamp(p-vec3(t.x),0.,1.),s);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 r = iResolution.xy;
    vec2 FC = fragCoord.xy;
    float t = iTime;
    vec4 o = vec4(0,0,0,1);
    float i,e,g,R,s;

    // Initialize ray direction for ray marching
    // Offset by 0.6 creates a perspective effect
    vec3 q,p,d=vec3(FC.xy/r-.6,1);

    // Main loop for ray marching
    // This loop advances the ray and accumulates color
    for(q.zy--;i++<99.;){
        // Accumulate 'density' along the ray
        e+=i/8e5;

        // Color accumulation using HSV
        // This creates a gradient effect based on the ray's position and properties
        o.rgb+=hsv(.6,R+g*.3,e*i/40.);

        // Ray marching step size
        s=4.;

        // Advance the ray
        p=q+=d*e*R*.2;

        // Accumulate vertical displacement, creating layered effect
        g+=p.y/s;

        // Transform space, creating repeating structures
        // R = distance from origin, creating spherical symmetry
        // sin(t) adds time-based animation
        // exp2(mod(-p.z,s)/R) creates exponential periodic structures along z-axis
        p=vec3((R=length(p))-.5+sin(t)*.02,exp2(mod(-p.z,s)/R)-.2,p);

        // Inner loop for detailed surface generation
        // This loop creates a noise-like pattern using trigonometric functions
        for(e=--p.y;s<1e3;s+=s)
            // This line generates a type of procedural noise
            // It's not classic FBM (Fractional Brownian Motion) or Perlin noise
            // Instead, it's a custom noise function using sinusoidal patterns
            // The dot product of sin and cos creates interference patterns
            // Dividing by 's' makes higher frequency contributions smaller
            e+=.03-abs(dot(sin(p.yzx*s),cos(p.xzz*s))/s*.6);

        // 'e' now contains the accumulated noise value
        // This affects both the color (in the outer loop) and the ray marching step
    }

    // Final color assignment
    fragColor = o;
}
