#include <glm/gtx/transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "ao/gl/frame.hpp"
#include "ao/gl/shader.hpp"

#include "ao/core/region.hpp"
#include "ao/core/tree.hpp"

#include "ao/render/heightmap.hpp"

////////////////////////////////////////////////////////////////////////////////
// Vertex shader
const std::string Frame::vert = R"(
#version 330

layout(location=0) in vec3 vertex_position;

uniform mat4 m;
out vec2 tex_coord;

void main()
{
    tex_coord = (vertex_position.xy + 1.0f) / 2.0f;
    gl_Position = m * vec4(vertex_position, 1.0f);
}
)";

// Fragment shader
const std::string Frame::frag = R"(
#version 330

uniform mat4 m;

in vec2 tex_coord;
uniform sampler2D depth;
uniform sampler2D norm;

out vec4 fragColor;

void main()
{
    float d = texture(depth, tex_coord).r;
    vec4 n = texture(norm, tex_coord) - vec4(0.5f);
    if (isinf(d))
    {
        discard;
    }
    else
    {
        float h = (d + 1.0f) / 2.0f;
        gl_FragDepth = d;
        fragColor = m * vec4(n.r, n.r, n.r, 1.0f);
    }
}
)";

////////////////////////////////////////////////////////////////////////////////

Frame::Frame(Tree* tree)
    : tree(tree), vs(Shader::compile(vert, GL_VERTEX_SHADER)),
      fs(Shader::compile(frag, GL_FRAGMENT_SHADER)), prog(Shader::link(vs, fs))
{
    assert(vs);
    assert(fs);
    assert(prog);

    glGenTextures(1, &depth);
    glGenTextures(1, &norm);

    glGenBuffers(1, &vbo);
    glGenVertexArrays(1, &vao);

    glBindVertexArray(vao);
    {
        GLfloat vertices[] = {-1.0f, -1.0f, 0.0f,
                               1.0f, -1.0f, 0.0f,
                               1.0f,  1.0f, 0.0f,
                              -1.0f,  1.0f, 0.0f};
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices),
                     vertices, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(GLfloat), (GLvoid*)0);
        glEnableVertexAttribArray(0);
    }
    glBindVertexArray(0);
}

Frame::~Frame()
{
    glDeleteTextures(1, &depth);
    glDeleteTextures(1, &norm);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
}

void Frame::draw(const glm::mat4& m) const
{
    if (!current.valid())
    {
        return;
    }

    // Active the shader
    glUseProgram(prog);

    // Bind the vertex array (which sets up VBO bindings)
    glBindVertexArray(vao);

    // Get the uniform location for the transform matrix
    GLint m_loc = glGetUniformLocation(prog, "m");

    // Calculate the appropriate transform matrix
    auto mat = m * glm::inverse(current.mat);
    glUniformMatrix4fv(m_loc, 1, GL_FALSE, glm::value_ptr(mat));

    // Bind depth and normal texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depth);
    glUniform1i(glGetUniformLocation(prog, "depth"), 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, norm);
    glUniform1i(glGetUniformLocation(prog, "norm"), 1);

    // Draw the quad!
    glEnable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisable(GL_DEPTH_TEST);

    glBindVertexArray(0);
}

////////////////////////////////////////////////////////////////////////////////

void Frame::render(const glm::mat4& m, size_t ni, size_t nj, size_t nk)
{
    next = Task(m, ni, nj, nk, 8);
    if (!future.valid())
    {
        startRender();
    }
}

void Frame::startRender()
{
    assert(!future.valid());
    assert(!pending.valid());

    // Start up the next render task
    if (next.valid())
    {
        // Create the target region for rendering
        // (allocated on the heap so it can persist)
        double div = 2.0 * next.level;
        Region* r = new Region({-1, 1}, {-1, 1}, {-1, 1},
                               next.ni/div, next.nj/div, next.nk/div);

        // Apply the matrix transform to the tree
        tree->setMatrix(glm::inverse(next.mat));

        // Then kick off an async render operation
        // (also responsible for cleaning up the Region allocated above)
        pending = next;
        next.reset();
        future = std::async(std::launch::async, [=](){
                auto depth = Heightmap::Render(tree, *r);
                auto shaded = Heightmap::Shade(tree, *r, depth);
                delete r;
                return std::make_pair(depth, shaded); });
    }
    // Schedule a refinement of the current render task
    else if (current.level > 1)
    {
        next = current;
        next.level /= 2;
        startRender();
    }
}

bool Frame::poll()
{
    if(!future.valid())
    {
        return false;
    }

    std::future_status status = future.wait_for(std::chrono::seconds(0));
    if (status == std::future_status::ready)
    {
        // Get the resulting matrix
        auto out = future.get();
        Eigen::ArrayXXf d = out.first.cast<float>().transpose();
        Image s = out.second.transpose();

        // Pack the Eigen matrices into an OpenGL texture
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4); // Floats are 4-byte aligned
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, depth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, d.rows(), d.cols(),
                0, GL_RED, GL_FLOAT, d.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, norm);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s.rows(), s.cols(),
                0, GL_RGBA, GL_UNSIGNED_BYTE, s.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

        // Swap tasks objects
        current = pending;
        pending.reset();

        // Attempt to kick off a new render
        startRender();

        return true;
    }
    else
    {
        return false;
    }
}