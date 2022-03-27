/*
 * Signed distance field shape fragment shader.
 *
 * MIT License, See LICENSE.md
 *
 * Copyright © 2013 Inigo Quilez
 * Copyright © 2016 Viktor Chlumsky
 * Copyright © 2019 Michael Clark
 *
 * shape shader support:
 *
 * - fill and stroke for contours with linear and quadratic segments
 * - fill and stroke for circles, ellipses, rectangles and rounded rectangles
 * - solid color fill, radial and axial gradient fill
 *
 * shape shader limitations:
 *
 * - simplistic n^2 contour search per fragment in complex contours
 * - stroke endcaps and joins degrade to distance to nearest contour
 * - join render bugs e.g. stroke joins on rounded rectangle corners
 *
 * shape shader todo:
 *
 * - tesselation, binning and root caching pre-pass in compute shader
 * - cubic contours, shape masks, stroke endcaps and stroke joins
 */

#version 150

//#extension GL_EXT_gpu_shader4 : enable

in vec4 v_color;
in vec2 v_uv0;
in float v_gamma;
in float v_shape;

uniform samplerBuffer tb_shape;
uniform samplerBuffer tb_edge;
uniform samplerBuffer tb_brush;

out vec4 outFragColor;

#define FLT_MAX 3.4028235e38
#define M_PI 3.1415926535897932384626433832795

#define Linear 2
#define Quadratic 3
#define Cubic 4
#define Rectangle 5
#define Circle 6
#define Ellipse 7
#define RoundedRectangle 8

#define Solid 1
#define Axial 2
#define Radial 3

struct Shape {
    int contour_offset;
    int contour_count;
    int edge_offset;
    int edge_count;
    vec2 offset;
    vec2 size;
    int fill_brush;
    int stroke_brush;
    float stroke_width;
    float stroke_mode;
};

struct Edge {
    int edge_type;
    vec2 p[4];
};

struct Brush {
    int brush_type;
    vec2 p[4];
    vec4 c[4];
};

/*
 * Copyright © 2013 Inigo Quilez, MIT License
 *
 * Analytical distance to an 2D ellipse, is more complicated than it seems.
 * Some steps through the derivation can be found in this article:
 *
 * http://iquilezles.org/www/articles/ellipsedist/ellipsedist.htm
 */

float sdEllipse( vec2 p, in vec2 ab )
{
    p = abs( p ); if( p.x > p.y ){ p=p.yx; ab=ab.yx; }

    float l = ab.y*ab.y - ab.x*ab.x;

    float m = ab.x*p.x/l;
    float n = ab.y*p.y/l;
    float m2 = m*m;
    float n2 = n*n;

    float c = (m2 + n2 - 1.0)/3.0;
    float c3 = c*c*c;

    float q = c3 + m2*n2*2.0;
    float d = c3 + m2*n2;
    float g = m + m*n2;

    float co;

    if( d<0.0 )
    {
        float h = acos(q/c3)/3.0;
        float s = cos(h);
        float t = sin(h)*sqrt(3.0);
        float rx = sqrt( -c*(s + t + 2.0) + m2 );
        float ry = sqrt( -c*(s - t + 2.0) + m2 );
        co = ( ry + sign(l)*rx + abs(g)/(rx*ry) - m)/2.0;
    }
    else
    {
        float h = 2.0*m*n*sqrt( d );
        float s = sign(q+h)*pow( abs(q+h), 1.0/3.0 );
        float u = sign(q-h)*pow( abs(q-h), 1.0/3.0 );
        float rx = -s - u - c*4.0 + 2.0*m2;
        float ry = (s - u)*sqrt(3.0);
        float rm = sqrt( rx*rx + ry*ry );
        co = (ry/sqrt(rm-rx) + 2.0*g/rm - m)/2.0;
    }

    float si = sqrt( 1.0 - co*co );

    vec2 r = ab * vec2(co,si);

    return length(r-p) * sign(r.y-p.y);
}

/*
 * Copyright © 2016 Viktor Chlumsky, MIT License
 *
 * Signed distance calculation for Quadratic and Cubic Bézier Curves:
 *
 * https://github.com/Chlumsky/msdfgen
 */

/* begin msdfgen port */

int solveQuadratic(out vec3 x, float a, float b, float c)
{
    if (abs(a) < 1e-14) {
        if (abs(b) < 1e-14) {
            if (c == 0)
                return -1;
            return 0;
        }
        x[0] = -c/b;
        return 1;
    }
    float dscr = b*b-4*a*c;
    if (dscr > 0) {
        dscr = sqrt(dscr);
        x[0] = (-b+dscr)/(2*a);
        x[1] = (-b-dscr)/(2*a);
        return 2;
    } else if (dscr == 0) {
        x[0] = -b/(2*a);
        return 1;
    } else
        return 0;
}

int solveCubicNormed(out vec3 x, float a, float b, float c)
{
    float a2 = a*a;
    float q  = (a2 - 3*b)/9;
    float r  = (a*(2*a2-9*b) + 27*c)/54;
    float r2 = r*r;
    float q3 = q*q*q;
    float A, B;
    if (r2 < q3) {
        float t = r/sqrt(q3);
        if (t < -1) t = -1;
        if (t > 1) t = 1;
        t = acos(t);
        a /= 3; q = -2*sqrt(q);
        x[0] = q*cos(t/3)-a;
        x[1] = q*cos((t+2*M_PI)/3)-a;
        x[2] = q*cos((t-2*M_PI)/3)-a;
        return 3;
    } else {
        A = -pow(abs(r)+sqrt(r2-q3), 1/3.);
        if (r < 0) A = -A;
        B = A == 0 ? 0 : q/A;
        a /= 3;
        x[0] = (A+B)-a;
        x[1] = -0.5*(A+B)-a;
        x[2] = 0.5*sqrt(3.)*(A-B);
        if (abs(x[2]) < 1e-14) {
            return 2;
        }
        return 1;
    }
}

int solveCubic(out vec3 x, float a, float b, float c, float d)
{
    if (abs(a) < 1e-14) {
        return solveQuadratic(x, b, c, d);
    } else {
        return solveCubicNormed(x, b/a, c/a, d/a);
    }
}

vec2 directionQuadratic(vec2 p[4], float cos_theta)
{
    vec2 tangent = mix(p[1]-p[0], p[2]-p[1], cos_theta);
    if (tangent.x == 0 && tangent.y == 0) {
        return p[2]-p[0];
    }
    return tangent;
}

vec2 directionCubic(vec2 p[4], float cos_theta)
{
    vec2 tangent = mix(mix(p[1]-p[0], p[2]-p[1], cos_theta), mix(p[2]-p[1], p[3]-p[2], cos_theta), cos_theta);
    if (tangent.x == 0 && tangent.y == 0) {
        if (cos_theta == 0) return p[2]-p[0];
        if (cos_theta == 1) return p[3]-p[1];
    }
    return tangent;
}

float cross(vec2 a, vec2 b) {
    return a.x*b.y-a.y*b.x;
}

int nonZeroSign(float p) { return p > 0 ? 1 : -1; }
int nonZeroSign(vec2 p) { return length(p) > 0 ? 1 : -1; }
int nonZeroSign(vec3 p) { return length(p) > 0 ? 1 : -1; }

vec2 getOrthonormal(vec2 p, bool polarity, bool allowZero)
{
    float len = length(p);
    if (len == 0) {
        return polarity ? vec2(0, allowZero ? 0 : 1) : vec2(0, allowZero ? 0 : -1);
    } else {
        return polarity ? vec2(-p.y/len, p.x/len) : vec2(p.y/len, -p.x/len);
    }
}

float sdLinear(vec2 origin, vec2 p[4], out float dir, out float cos_theta)
{
    vec2 aq = origin-p[0];
    vec2 ab = p[1]-p[0];
    cos_theta = dot(aq, ab)/dot(ab, ab);
    vec2 eq = p[cos_theta > .5 ? 1 : 0]-origin;
    float endpointDistance = length(eq);
    if (cos_theta > 0 && cos_theta < 1) {
        float orthoDistance = dot(getOrthonormal(ab, false, false), aq);
        if (abs(orthoDistance) < endpointDistance) {
            dir = 0;
            return orthoDistance;
        }
    }
    dir = abs(dot(normalize(ab), normalize(eq)));
    return nonZeroSign(cross(aq, ab))*endpointDistance;
}

float sdQuadratic(vec2 origin, vec2 p[4], out float dir, out float cos_theta)
{
    vec2 qa = p[0]-origin;
    vec2 ab = p[1]-p[0];
    vec2 br = p[2]-p[1]-ab;
    float a = dot(br, br);
    float b = 3*dot(ab, br);
    float c = 2*dot(ab, ab)+dot(qa, br);
    float d = dot(qa, ab);
    vec3 t;
    int solutions = solveCubic(t, a, b, c, d);

    vec2 epDir = directionQuadratic(p, 0);
    float minDistance = nonZeroSign(cross(epDir, qa))*length(qa); // distance from A
    cos_theta = -dot(qa, epDir)/dot(epDir, epDir);
    {
        epDir = directionQuadratic(p, 1);
        float distance = nonZeroSign(cross(epDir, p[2]-origin))*length(p[2]-origin); // distance from B
        if (abs(distance) < abs(minDistance)) {
            minDistance = distance;
            cos_theta = dot(origin-p[1], epDir)/dot(epDir, epDir);
        }
    }
    for (int i = 0; i < solutions; ++i) {
        if (t[i] > 0 && t[i] < 1) {
            vec2 qe = p[0]+2*t[i]*ab+t[i]*t[i]*br-origin;
            float distance = nonZeroSign(cross(p[2]-p[0], qe))*length(qe);
            if (abs(distance) <= abs(minDistance)) {
                minDistance = distance;
                cos_theta = t[i];
            }
        }
    }

    if (cos_theta >= 0 && cos_theta <= 1) {
        dir = 0;
        return minDistance;
    }
    if (cos_theta < .5) {
        dir = abs(dot(normalize(directionQuadratic(p, 0)), normalize(qa)));
        return minDistance;
    }
    else {
        dir = abs(dot(normalize(directionQuadratic(p, 1)), normalize(p[2]-origin)));
        return minDistance;
    }
}

/* end msdfgen port */

float sdCubic(vec2 origin, vec2 p[4], out float dir, out float cos_theta)
{
    /* cubic contours have not been implemented */
    dir = 0.;
    cos_theta = 0.;
    return 0.;
}

float sdRect(vec2 center, vec2 halfSize, vec2 origin)
{
    vec2 edgeDist = abs(origin - center) - halfSize;
    float outsideDist = length(max(edgeDist, 0));
    float insideDist = min(max(edgeDist.x, edgeDist.y), 0);
    return -outsideDist - insideDist;
}

float sdRectangle(vec2 origin, vec2 p[4], out float dir, out float cos_theta)
{
    vec2 center = p[0], halfSize = p[1];
    return sdRect(center, halfSize, origin);
}

float sdCircle(vec2 origin, vec2 p[4], out float dir, out float cos_theta)
{
    vec2 center = p[0];
    float radius = p[1][0];
    return radius - length(origin - center);
}

float sdEllipse(vec2 origin, vec2 p[4], out float dir, out float cos_theta)
{
    vec2 center = p[0], radius = p[1];
    return sdEllipse(origin-center, radius);
}

float sdRoundedRectangle(vec2 origin, vec2 p[4], out float dir, out float cos_theta)
{
    vec2 center = p[0], halfSize = p[1];
    float radius = p[2].x;
    vec2 r1 = halfSize - vec2(radius,0);
    vec2 r2 = halfSize - vec2(0,radius);
    float d1 = sdRect(center, r1, origin);
    float d2 = sdRect(center, r2, origin);
    float c1 = radius - length(abs(origin - center) - halfSize + radius);
    return max(max(d1,d2),c1);
}

/*
 * Shape, Edge and Brush structure serialization from texture buffers.
 *
 * shader depends on OpenGL 3.2 baseline features to support macOS which
 * necessitates use of texelFetch to load structures due to lack of SSBOs.
 */

void getShape(out Shape shape, int shape_num)
{
    int o = shape_num * 12;
    shape.contour_offset =   int(texelFetch(tb_shape, o + 0).r);
    shape.contour_count =    int(texelFetch(tb_shape, o + 1).r);
    shape.edge_offset =      int(texelFetch(tb_shape, o + 2).r);
    shape.edge_count =       int(texelFetch(tb_shape, o + 3).r);
    shape.offset =      vec2(texelFetch(tb_shape, o + 4).r,
                             texelFetch(tb_shape, o + 5).r);
    shape.size =        vec2(texelFetch(tb_shape, o + 6).r,
                             texelFetch(tb_shape, o + 7).r);
    shape.fill_brush =   int(texelFetch(tb_shape, o + 8).r);
    shape.stroke_brush = int(texelFetch(tb_shape, o + 9).r);
    shape.stroke_width =     texelFetch(tb_shape, o + 10).r;
    shape.stroke_mode =      texelFetch(tb_shape, o + 11).r;
}

void getEdge(out Edge edge, int edge_num)
{
    int o = edge_num * 9;
    edge.edge_type = int(texelFetch(tb_edge, o + 0).r);
    edge.p[0] = vec2(texelFetch(tb_edge, o + 1).r, texelFetch(tb_edge, o + 2).r);
    edge.p[1] = vec2(texelFetch(tb_edge, o + 3).r, texelFetch(tb_edge, o + 4).r);
    edge.p[2] = vec2(texelFetch(tb_edge, o + 5).r, texelFetch(tb_edge, o + 6).r);
    edge.p[3] = vec2(texelFetch(tb_edge, o + 7).r, texelFetch(tb_edge, o + 8).r);
}

void getBrush(out Brush brush, int brush_num)
{
    int o = brush_num * 25;
    brush.brush_type = int(texelFetch(tb_brush, o + 0).r);
    brush.p[0] = vec2(texelFetch(tb_brush, o + 1).r, texelFetch(tb_brush, o + 2).r);
    brush.p[1] = vec2(texelFetch(tb_brush, o + 3).r, texelFetch(tb_brush, o + 4).r);
    brush.p[2] = vec2(texelFetch(tb_brush, o + 5).r, texelFetch(tb_brush, o + 6).r);
    brush.p[3] = vec2(texelFetch(tb_brush, o + 7).r, texelFetch(tb_brush, o + 8).r);
    brush.c[0] = vec4(texelFetch(tb_brush, o + 9).r,
                      texelFetch(tb_brush, o + 10).r,
                      texelFetch(tb_brush, o + 11).r,
                      texelFetch(tb_brush, o + 12).r);
    brush.c[1] = vec4(texelFetch(tb_brush, o + 13).r,
                      texelFetch(tb_brush, o + 14).r,
                      texelFetch(tb_brush, o + 15).r,
                      texelFetch(tb_brush, o + 16).r);
    brush.c[2] = vec4(texelFetch(tb_brush, o + 17).r,
                      texelFetch(tb_brush, o + 18).r,
                      texelFetch(tb_brush, o + 19).r,
                      texelFetch(tb_brush, o + 20).r);
    brush.c[3] = vec4(texelFetch(tb_brush, o + 21).r,
                      texelFetch(tb_brush, o + 22).r,
                      texelFetch(tb_brush, o + 23).r,
                      texelFetch(tb_brush, o + 24).r);
}

float getDistanceEdge(Edge edge, vec2 origin, out float dir, out float cos_theta)
{
    switch(edge.edge_type) {
    case Linear:    return sdLinear   (origin, edge.p, dir, cos_theta);
    case Quadratic: return sdQuadratic(origin, edge.p, dir, cos_theta);
    case Cubic:     return sdCubic    (origin, edge.p, dir, cos_theta);
    case Rectangle: return sdRectangle(origin, edge.p, dir, cos_theta);
    case Circle:    return sdCircle   (origin, edge.p, dir, cos_theta);
    case Ellipse:   return sdEllipse  (origin, edge.p, dir, cos_theta);
    case RoundedRectangle: return sdRoundedRectangle(origin, edge.p, dir, cos_theta);
    }
    return FLT_MAX; /* not reached */
}

float getDistanceShape(Shape shape, vec2 origin, out float dir, out float cos_theta)
{
    Edge edge;
    float distance, minDistance = FLT_MAX, minDir = FLT_MAX;
    for (int i = 0; i < shape.edge_count; i++) {
        getEdge(edge, shape.edge_offset + i);
        distance = getDistanceEdge(edge, origin, dir, cos_theta);
        if (abs(distance) < abs(minDistance) ||
            abs(distance) == abs(minDistance) && dir < minDir)
        {
            minDistance = distance;
            minDir = dir;
        }
    }
    return minDistance;
}

vec4 brushColorSolid(Brush brush, vec2 origin)
{
    return brush.c[0];
}

vec4 brushColorRadial(Brush brush, vec2 origin)
{
    float l1 = length(brush.p[0]);
    float l2 = length(brush.p[1]);
    float t = (length(origin) - l1)/(l2-l1);
    t = clamp(t, 0, 1);
    return (1-t) * brush.c[0] + t * brush.c[1];
}

vec4 brushColorAxial(Brush brush, vec2 origin)
{
    float x0 = brush.p[0].x, y0 = brush.p[0].y;
    float x1 = brush.p[1].x, y1 = brush.p[1].y;
    float t = ((x1 - x0) * (origin.x - x0) + (y1 - y0) * (origin.y - y0)) /
        ((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    t = clamp(t, 0, 1);
    return (1-t) * brush.c[0] + t * brush.c[1];
}

vec4 getColorBrush(int brush_num, vec2 origin, vec4 default_color)
{
    if (brush_num >= 0) {
        Brush brush;
        getBrush(brush, brush_num);
        switch(brush.brush_type) {
        case Solid:  return brushColorSolid(brush, origin);
        case Radial: return brushColorRadial(brush, origin);
        case Axial:  return brushColorAxial(brush, origin);
        }
    }
    return default_color;
}

/*
 * Signed distance field shape fragment shader main.
 */

void main()
{
    Shape shape;
    getShape(shape, int(v_shape));

    vec4 fill_color = getColorBrush(shape.fill_brush, v_uv0.xy, v_color);
    vec4 stroke_color = getColorBrush(shape.stroke_brush, v_uv0.xy, v_color);

    float dir, cos_theta;
    float distance = getDistanceShape(shape, v_uv0.xy, dir, cos_theta);

    if (shape.stroke_mode > 0) {
        distance = -abs(distance);
    }

    float dx = dFdx( v_uv0.x );
    float dy = dFdy( v_uv0.y );
    float ps = sqrt(dx*dx + dy*dy);
    float w = shape.stroke_width/2.0;
    float alpha = smoothstep(-w-ps, -w+ps, distance);
    vec4 color = shape.stroke_width == 0 ? fill_color :
        mix(stroke_color, fill_color, smoothstep(w-ps, w+ps, distance));

    outFragColor = vec4(pow(color.rgb, vec3(1.0/v_gamma)), alpha * color.a);
}
