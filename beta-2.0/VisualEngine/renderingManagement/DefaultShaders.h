#pragma once

static const char* defaultVertSrc = R"(
#version 430 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
layout (location = 2) in vec3 aNormal;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat3 normalMatrix;

out vec3 FragPos;
out vec2 TexCoord;
out vec3 Normal;

void main() {
    FragPos = vec3(model * vec4(aPos, 1.0));
    TexCoord = aTexCoord;
    Normal = normalMatrix * aNormal;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
)";

static const char* defaultFragSrc = R"(
#version 430 core
in vec3 FragPos;
in vec2 TexCoord;
in vec3 Normal;

uniform vec3 lightPos;
uniform vec3 lightColor;
uniform float ambientStrength;
uniform float specularStrength;
uniform int shininess;
uniform vec3 viewPos;
uniform vec3 objectColor;
uniform sampler2D textureSampler;
uniform bool useTexture;
uniform float alpha;
uniform float brightness;
uniform bool fogEnabled;
uniform vec3 fogColor;
uniform float fogStart;
uniform float fogEnd;

out vec4 FragColor;

void main() {
    float fogDist = 0.0;
    if (fogEnabled) {
        vec3 fogD = viewPos - FragPos;
        fogD.y *= 2.0; // vertical fog reaches saturation at half horizontal range
        fogDist = length(fogD);
        if (fogDist > fogEnd) discard;
    }

    vec3 baseColor;
    if (useTexture) {
        baseColor = texture(textureSampler, TexCoord).rgb;
    } else {
        baseColor = objectColor;
    }

    vec3 ambient = ambientStrength * lightColor;

    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * lightColor;

    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    vec3 specular = specularStrength * spec * lightColor;

    vec3 result = (ambient + diffuse + specular) * baseColor;
    result *= brightness;
    if (fogEnabled) {
        float fogFactor = smoothstep(fogStart, fogEnd, fogDist);
        result = mix(result, fogColor, fogFactor);
    }
    FragColor = vec4(result, alpha);
}
)";

// Vertex-colored shader: per-vertex RGB instead of texture/uniform color.
// Same lighting model as defaultFragSrc; reads color straight from the vertex.
// Used for meshes built from voxel models with COLOR_0 streams (exported GLBs,
// or any mesh that wants per-vertex flat colors).
static const char* vertexColoredVertSrc = R"(
#version 430 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec3 aColor;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat3 normalMatrix;

out vec3 FragPos;
out vec3 Normal;
out vec3 VertColor;

void main() {
    FragPos = vec3(model * vec4(aPos, 1.0));
    Normal = normalMatrix * aNormal;
    VertColor = aColor;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
)";

static const char* vertexColoredFragSrc = R"(
#version 430 core
in vec3 FragPos;
in vec3 Normal;
in vec3 VertColor;

uniform vec3 lightPos;
uniform vec3 lightColor;
uniform float ambientStrength;
uniform float specularStrength;
uniform int shininess;
uniform vec3 viewPos;
uniform float alpha;
uniform float brightness;
uniform bool fogEnabled;
uniform vec3 fogColor;
uniform float fogStart;
uniform float fogEnd;

out vec4 FragColor;

void main() {
    float fogDist = 0.0;
    if (fogEnabled) {
        vec3 fogD = viewPos - FragPos;
        fogD.y *= 2.0; // vertical fog reaches saturation at half horizontal range
        fogDist = length(fogD);
        if (fogDist > fogEnd) discard;
    }

    vec3 ambient = ambientStrength * lightColor;

    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * lightColor;

    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    vec3 specular = specularStrength * spec * lightColor;

    vec3 result = (ambient + diffuse + specular) * VertColor;
    result *= brightness;
    if (fogEnabled) {
        float fogFactor = smoothstep(fogStart, fogEnd, fogDist);
        result = mix(result, fogColor, fogFactor);
    }
    FragColor = vec4(result, alpha);
}
)";