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

#endif
