// kb_text_shape's implementation (zlib licence), compiled apart from gesso's own code: it reads font
// tables through unaligned pointers by design, which the undefined-behaviour sanitizer of debug builds
// would stop on, so this file is built without it.
#define KB_TEXT_SHAPE_IMPLEMENTATION
#include "kb_text_shape.h"
