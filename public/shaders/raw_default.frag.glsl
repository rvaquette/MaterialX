precision highp float;

uniform vec3 u_baseColor;
uniform vec3 u_lightDir;
uniform vec3 u_lightColor;
uniform float u_time;
uniform vec3 cameraPosition;

varying vec3 vNormal;
varying vec3 vWorldPos;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(u_lightDir);
    vec3 V = normalize(cameraPosition - vWorldPos);
    vec3 H = normalize(L + V);

    float ndotl = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 64.0);

    float pulse = 0.95 + 0.05 * sin(u_time * 2.0);
    vec3 color = u_baseColor * (0.15 + ndotl) * pulse;
    color += u_lightColor * spec * 0.35;

    gl_FragColor = vec4(color, 1.0);
}
