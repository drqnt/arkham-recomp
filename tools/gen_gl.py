#!/usr/bin/env python3
"""Generate runtime/gl_gen.c: one shim per OpenGL function the game imports, typed from the SDK headers."""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
from game_path import game_binary
from macho import MachO

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = dict(os.environ, DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer")
SDK = subprocess.run(["xcrun", "--sdk", "macosx", "--show-sdk-path"], capture_output=True, text=True, env=ENV).stdout.strip()
HDR = f"{SDK}/System/Library/Frameworks/OpenGL.framework/Headers"

# Pointer arguments that may be a byte offset into a bound buffer rather than a real address.
BOUND = {
    "glVertexAttribPointer": "GL_ARRAY_BUFFER_BINDING", "glVertexAttribIPointer": "GL_ARRAY_BUFFER_BINDING",
    "glVertexPointer": "GL_ARRAY_BUFFER_BINDING", "glNormalPointer": "GL_ARRAY_BUFFER_BINDING",
    "glColorPointer": "GL_ARRAY_BUFFER_BINDING", "glTexCoordPointer": "GL_ARRAY_BUFFER_BINDING",
    "glFogCoordPointerEXT": "GL_ARRAY_BUFFER_BINDING", "glSecondaryColorPointerEXT": "GL_ARRAY_BUFFER_BINDING",
    "glDrawElements": "GL_ELEMENT_ARRAY_BUFFER_BINDING", "glDrawRangeElements": "GL_ELEMENT_ARRAY_BUFFER_BINDING",
    "glDrawElementsInstanced": "GL_ELEMENT_ARRAY_BUFFER_BINDING", "glDrawElementsBaseVertex": "GL_ELEMENT_ARRAY_BUFFER_BINDING",
    "glDrawElementsInstancedBaseVertex": "GL_ELEMENT_ARRAY_BUFFER_BINDING", "glDrawRangeElementsBaseVertex": "GL_ELEMENT_ARRAY_BUFFER_BINDING",
    "glTexImage1D": "GL_PIXEL_UNPACK_BUFFER_BINDING", "glTexImage2D": "GL_PIXEL_UNPACK_BUFFER_BINDING", "glTexImage3D": "GL_PIXEL_UNPACK_BUFFER_BINDING",
    "glTexSubImage1D": "GL_PIXEL_UNPACK_BUFFER_BINDING", "glTexSubImage2D": "GL_PIXEL_UNPACK_BUFFER_BINDING", "glTexSubImage3D": "GL_PIXEL_UNPACK_BUFFER_BINDING",
    "glCompressedTexImage1D": "GL_PIXEL_UNPACK_BUFFER_BINDING", "glCompressedTexImage2D": "GL_PIXEL_UNPACK_BUFFER_BINDING", "glCompressedTexImage3D": "GL_PIXEL_UNPACK_BUFFER_BINDING",
    "glCompressedTexSubImage1D": "GL_PIXEL_UNPACK_BUFFER_BINDING", "glCompressedTexSubImage2D": "GL_PIXEL_UNPACK_BUFFER_BINDING", "glCompressedTexSubImage3D": "GL_PIXEL_UNPACK_BUFFER_BINDING",
    "glReadPixels": "GL_PIXEL_PACK_BUFFER_BINDING", "glGetTexImage": "GL_PIXEL_PACK_BUFFER_BINDING", "glGetCompressedTexImage": "GL_PIXEL_PACK_BUFFER_BINDING",
    "glDrawArraysIndirect": "GL_DRAW_INDIRECT_BUFFER_BINDING", "glDrawElementsIndirect": "GL_DRAW_INDIRECT_BUFFER_BINDING",
}
# Implemented by hand in shims_gl.c.
MANUAL = {"glGetString", "glGetStringi", "glMapBuffer", "glMapBufferRange", "glUnmapBuffer", "glFlushMappedBufferRange",
          "glFlushMappedBufferRangeAPPLE", "glShaderSource", "glFenceSync", "glClientWaitSync", "glDeleteSync",
          "glTransformFeedbackVaryings", "glFinish", "glCompileShader", "glLinkProgram",
          "glBindBuffer", "glBindVertexArray", "glDeleteBuffers"}

INTS = {"GLenum", "GLboolean", "GLbitfield", "GLbyte", "GLshort", "GLint", "GLubyte", "GLushort", "GLuint", "GLsizei",
        "GLchar", "GLcharARB", "GLenum", "GLhalf", "GLhandleARB", "GLfixed"}
SIGNED_PTRSIZE = {"GLintptr", "GLsizeiptr", "GLintptrARB", "GLsizeiptrARB"}


def parse_prototypes(text, wanted):
    protos = {}
    pattern = re.compile(r"(?:extern|GLAPI)\s+([^;()]*?)\s*(?:APIENTRY\s+)?\b(gl[A-Z]\w*)\s*\(([^;]*?)\)\s*[^;()]*(?:\([^;()]*\))?\s*;", re.S)
    for m in pattern.finditer(text):
        ret, name, args = m.group(1).replace("APIENTRY", "").strip(), m.group(2), m.group(3)
        if name not in wanted or name in protos:
            continue
        protos[name] = (ret, args.strip())
    return protos


def classify(tok):
    tok = re.sub(r"\bconst\b", "", tok).strip()
    star = tok.count("*")
    base = tok.replace("*", " ").split()
    tyname = base[0] if base else "void"
    if tyname in ("struct",):
        tyname = base[1]
    if star >= 2:
        return ("pp", tok)
    if star == 1:
        return ("p", tok)
    if tyname == "GLfloat" or tyname == "GLclampf":
        return ("f", "float")
    if tyname == "GLdouble" or tyname == "GLclampd":
        return ("d", "double")
    if tyname in ("GLint64", "GLuint64", "GLint64EXT", "GLuint64EXT"):
        return ("q", "unsigned long long")
    if tyname in SIGNED_PTRSIZE:
        return ("iptr", "long")
    if tyname == "GLsync":
        return ("sync", "void *")
    if tyname == "void":
        return ("v", "void")
    return ("i", "unsigned int")


def main():
    m = MachO(game_binary())
    gl_ord = next(i + 1 for i, d in enumerate(m.dylibs) if d.endswith("OpenGL.framework/Versions/A/OpenGL"))
    wanted = sorted({b.symbol[1:] for b in m.binds if b.ordinal == gl_ord and b.symbol.startswith("_gl")})
    text = ""
    for h in ("gl3.h", "gl3ext.h", "gl.h", "glext.h"):
        p = f"{HDR}/{h}"
        if os.path.exists(p):
            text += open(p, errors="replace").read() + "\n"
    protos = parse_prototypes(text, set(wanted))
    out = ['// Generated by tools/gen_gl.py from the SDK OpenGL headers. Do not edit.',
           '#include "shim.h"', '#include "shim_reg.h"', '#include "shims_gl.h"', '#include <dlfcn.h>', '#include <stdio.h>', '']
    regs = []
    missing = []
    for name in wanted:
        if name in MANUAL:
            regs.append(name)
            continue
        if name not in protos:
            missing.append(name)
            out.append(f'SHIM({name}) {{ static int w; if (!w) {{ w = 1; fprintf(stderr, "[gl] no prototype for {name}\\n"); }} }}')
            regs.append(name)
            continue
        ret, args = protos[name]
        arglist = [] if args in ("void", "") else [a.strip() for a in args.split(",")]
        slot = 0
        ctypes, exprs = [], []
        bad = False
        bound = BOUND.get(name)
        n_args = len(arglist)
        for k, a in enumerate(arglist):
            # Strip the parameter name when present.
            t = re.sub(r"\b\w+$", "", a).strip() if not a.rstrip().endswith("*") and len(a.split()) > 1 else a
            kind, cty = classify(t)
            if kind == "i":
                ctypes.append("unsigned int")
                exprs.append(f"ARG({slot})")
                slot += 1
            elif kind == "f":
                ctypes.append("float")
                exprs.append(f"ARGF({slot})")
                slot += 1
            elif kind == "d":
                ctypes.append("double")
                exprs.append(f"ARGD({slot})")
                slot += 2
            elif kind == "q":
                ctypes.append("unsigned long long")
                exprs.append(f"ARGQ({slot})")
                slot += 2
            elif kind == "iptr":
                ctypes.append("long")
                exprs.append(f"(long)(i32)ARG({slot})")
                slot += 1
            elif kind == "sync":
                ctypes.append("void *")
                exprs.append(f"rt_handle_get(ARG({slot}))")
                slot += 1
            elif kind == "p":
                ctypes.append("void *")
                # The pointer-or-offset argument is always the last pointer of these calls.
                is_last_ptr = bound and all(classify(re.sub(r"\b\w+$", "", x).strip())[0] != "p" for x in arglist[k + 1:])
                if is_last_ptr:
                    exprs.append(f"gl_ptr_or_offset(ARG({slot}), {bound})")
                else:
                    exprs.append(f"NP(ARG({slot}))")
                slot += 1
            else:
                bad = True
        rk, _ = classify(ret)
        if bad or rk in ("p", "pp", "q", "sync"):
            out.append(f'SHIM({name}) {{ rt_trap(c, G32(c->esp), "unsupported signature: {name}"); }}')
            regs.append(name)
            continue
        rty = {"v": "void", "i": "unsigned int", "f": "float", "d": "double", "iptr": "long"}[rk]
        sig = ", ".join(ctypes) if ctypes else "void"
        call = f"f({', '.join(exprs)})"
        body = f"static {rty} (*f)({sig}); if (!f) {{ f = dlsym(RTLD_DEFAULT, \"{name}\"); if (!f) {{ gl_missing(\"{name}\"); return; }} }} "
        if rk == "v":
            body += f"{call};"
        elif rk in ("f", "d"):
            body += f"RETF({call});"
        else:
            body += f"RET({call});"
        count = "rt_gl_draws++; " if name.startswith("glDraw") and not name.startswith("glDrawBuffer") else ""
        out.append(f"SHIM({name}) {{ {count}{body} }}")
        regs.append(name)
    out.append("")
    out.append("const ShimReg rt_shims_gl_gen[] = {")
    for name in regs:
        if name not in MANUAL:
            out.append(f"    R({name})")
    out.append("    {NULL, NULL},\n};")
    with open(f"{ROOT}/runtime/gl_gen.c", "w") as fh:
        fh.write("\n".join(out) + "\n")
    print(f"gl functions {len(wanted)}, generated {len(regs) - len(set(regs) & MANUAL)}, manual {len(set(regs) & MANUAL)}, no prototype: {missing}")


if __name__ == "__main__":
    main()
