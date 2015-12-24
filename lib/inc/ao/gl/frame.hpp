#pragma once

#include "ao/gl/core.hpp"

/*
 *  The Frame class contains and draws a rendered Tree texture
 */
class Frame
{
public:
    explicit Frame();
    ~Frame();

    void Draw();

protected:
    // Texture objects
    GLuint depth;
    GLuint normal;

    // Vertex buffer object
    GLuint vbo;

    // Vertex array object
    GLuint vao;
};
