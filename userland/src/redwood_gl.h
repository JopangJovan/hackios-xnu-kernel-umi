/*
 * What src/eagl.c asks of the OpenGL ES library behind it besides the gl* functions: see
 * src/gl_osmesa.c, the one implementation there is.
 */
#ifndef REDWOOD_GL_H
#define REDWOOD_GL_H

struct redwood_gl;

struct redwood_gl *redwood_gl_create(int version);              /* a context of OpenGL ES 2 or 3; NULL: none to be had */
int redwood_gl_make_current(struct redwood_gl *gl);             /* ... the calling thread's (NULL: none); 0 on failure */
void redwood_gl_destroy(struct redwood_gl *gl);
const char *redwood_gl_library(void);                           /* for the log */

/*
 * The texture bound to GL_TEXTURE_2D made of an IOSurface's own memory: what is drawn into the
 * texture is in the surface, what is in the surface is what the texture shows, and nobody copies.
 * For a library whose driver can do that (a GPU's) and a surface it can do it with. 0: not
 * done; the texture is to be filled and read back with OpenGL ES calls.
 */
int redwood_gl_surface_texture(struct redwood_gl *gl, uint32_t surface);

#endif
