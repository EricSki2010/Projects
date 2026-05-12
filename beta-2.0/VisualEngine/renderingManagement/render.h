#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <memory>
#include <unordered_map>
#include <string>

struct PointLight {
    glm::vec3 position;
    glm::vec3 color;
    float ambientStrength;
    float specularStrength;
    int shininess;
};

struct Fog {
    bool enabled = false;
    glm::vec3 color = glm::vec3(0.55f, 0.70f, 0.95f);
    float start = 50.0f;
    float end = 150.0f;
};

class Shader {
public:
    unsigned int program;

    Shader(const char* vertexSrc, const char* fragmentSrc);
    ~Shader();
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    void use() const;
    unsigned int getID() const;
    int loc(const char* name);
private:
    unsigned int compile(const char* source, GLenum type);
    std::unordered_map<std::string, int> uniformCache;
};

class Scene {
public:
    glm::mat4 projection;
    glm::mat4 view;
    PointLight light;
    Fog fog;
    float farPlane = 500.0f;

    Scene(float aspectRatio);
    glm::mat3 getNormalMatrix(const glm::mat4& model) const;
    void uploadStaticUniforms(Shader& shader) const;
    void uploadFrameUniforms(Shader& shader, const glm::mat4& model) const;
};

class Texture {
public:
    unsigned int id;

    Texture(const char* filepath, bool pixelated = false);
    Texture(const unsigned char* data, int dataSize, bool pixelated = false);
    Texture(const unsigned char* pixels, int width, int height, int channels);
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    void bind(int unit = 0) const;
};

class Mesh {
public:
    unsigned int VAO, VBO, EBO;
    int indexCount;
    Texture* texture;
    glm::vec3 color = glm::vec3(0.8f);
    bool vertexColored = false; // true when built from pos+normal+color stream

    // Arena-slice mode: this Mesh does NOT own VAO/VBO/EBO — it references a
    // shared VAO (from a chunk arena) and draws via glDrawElementsBaseVertex
    // at the given byte/vertex offsets. Destructor skips glDelete in this mode.
    // Caller is responsible for keeping the underlying arena alive.
    bool   arenaSlice          = false;
    size_t arenaIndexByteOffset = 0;
    int    arenaBaseVertex      = 0;

    Mesh(float* vertices, int vertexCount, unsigned int* indices, int indexCount);
    Mesh(float* verticesWithNormals, int vertexCount, unsigned int* indices, int indexCount, bool hasNormals);
    ~Mesh();

    // Vertex-colored mesh factory. Input is interleaved pos3 + normal3 + color3
    // (9 floats per vertex). Built mesh must be drawn with the engine's
    // vertex-colored shader (ctx.vcShader).
    static std::unique_ptr<Mesh> createVertexColored(const float* verticesPosNormalColor,
                                                     int vertexCount,
                                                     const unsigned int* indices,
                                                     int indexCount);

    // Arena-slice factory. Wraps a draw of an arena's shared VAO with offsets
    // — does NOT allocate any GL resources. Use when many small meshes share
    // a single VBO/EBO/VAO (e.g. chunk streaming) to avoid per-mesh glGen +
    // glBufferData driver-pool churn.
    static std::unique_ptr<Mesh> createArenaSlice(unsigned int sharedVAO,
                                                  size_t indexByteOffset,
                                                  int indexCount,
                                                  int baseVertex);

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    void setTexture(Texture* tex);
    void setColor(glm::vec3 col);
    void draw(Shader& shader);
private:
    Mesh() = default; // used by createVertexColored / createArenaSlice
    void computeNormals(float* vertices, int vertexCount, unsigned int* indices, int indexCount,
                        float* outBuffer);
};
