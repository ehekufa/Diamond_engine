// Diamond Engine — GLSL program compilation helper.
#include "renderer.h"

#include <cstdlib>
#include <cstring>

extern "C" {
#include "diamond/dlog.h"
#include "diamond/dutil.h"
}

namespace diamond {

namespace {

GLuint compile_stage(GLenum type, const GLchar *source, const char *label) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512] = {0};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        dlog_write(DLOG_ERROR, "gl", "%s shader compile failed: %s", label, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

bool gl_program_link(GlProgram *program, const GLchar *vertexSource,
                     const GLchar *fragmentSource, const char *label,
                     int *esVersionOut) {
    if (!program || !vertexSource || !fragmentSource) return false;
    GLuint vertex = compile_stage(GL_VERTEX_SHADER, vertexSource, label);
    if (!vertex) return false;
    GLuint fragment = compile_stage(GL_FRAGMENT_SHADER, fragmentSource, label);
    if (!fragment) { glDeleteShader(vertex); return false; }
    GLuint id = glCreateProgram();
    if (!id) { glDeleteShader(vertex); glDeleteShader(fragment); return false; }
    glAttachShader(id, vertex);
    glAttachShader(id, fragment);
    glLinkProgram(id);
    GLint status = 0;
    glGetProgramiv(id, GL_LINK_STATUS, &status);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (!status) {
        char log[512] = {0};
        glGetProgramInfoLog(id, sizeof(log) - 1, nullptr, log);
        dlog_write(DLOG_ERROR, "gl", "%s program link failed: %s", label, log);
        glDeleteProgram(id);
        return false;
    }
    program->id = id;
    program->uMvp = glGetUniformLocation(id, "uMvp");
    program->uModel = glGetUniformLocation(id, "uModel");
    program->uColor = glGetUniformLocation(id, "uColor");
    program->uLight = glGetUniformLocation(id, "uLight");
    program->uCamPos = glGetUniformLocation(id, "uCamPos");
    program->uTex = glGetUniformLocation(id, "uTex");
    program->uUseTexture = glGetUniformLocation(id, "uUseTexture");
    if (esVersionOut) *esVersionOut = 3;
    return true;
}

void gl_program_release(GlProgram *program) {
    if (program && program->id) {
        glDeleteProgram(program->id);
        program->id = 0;
    }
}

GLuint gl_program_id(const GlProgram *program) { return program ? program->id : 0; }

} // namespace diamond
