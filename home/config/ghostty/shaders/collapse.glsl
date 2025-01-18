void mainImage( out vec4 O, vec2 I)
{
    vec2 r=iResolution.xy,
    p=(I+I-r)/r.y*mat2(4,-1,1,4),
    v;
    O*=0.;
    for(float t=iTime,i; i++<50.;
        O+=(cos(sin(i+t/3.+p.x*.1)*vec4(7,4,2,1))+1.)
        /length(max(v,v/vec2(5e1*texture(iChannel0,p/50.+.1*vec2(t,i)).r,2))))
        v=p+cos(i*i+t+p.x*.2+vec2(8,0))*3.;
    O=tanh(O*O/3e4);
}
