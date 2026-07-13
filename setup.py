"""pyffish-spell: Python bindings for the Spell-Stockfish rules layer.

Builds the rules-only closure (no threads/TT/search/NNUE runtime) into a
single extension module `pyffish_spell`. Surface documented in
docs/bindings-port-plan.md; deviations from upstream pyffish are listed in
src/pyffish.cpp.
"""

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

SOURCES = [
    "src/pyffish.cpp",
    "src/position.cpp",
    "src/movegen.cpp",
    "src/bitboard.cpp",
    "src/attacks.cpp",
    "src/notation.cpp",
    "src/misc.cpp",
    "src/memory.cpp",
    "src/spell_params.cpp",
    "src/tune.cpp",
    "src/ucioption.cpp",
]

MACROS = [("NDEBUG", None), ("SPELL_RULES_ONLY", None)]


class BuildExt(build_ext):
    """Pick C++17 flags per actual compiler (MSVC vs GCC/Clang/MinGW)."""

    def build_extensions(self):
        if self.compiler.compiler_type == "msvc":
            args = ["/std:c++17", "/EHsc", "/O2"]
        else:
            args = ["-std=c++17", "-O2"]
        for ext in self.extensions:
            ext.extra_compile_args = args
        super().build_extensions()


setup(
    name="pyffish-spell",
    version="0.1.0",
    description="Spell Chess rules bindings (Spell-Stockfish), pyffish-compatible surface",
    long_description=__doc__,
    author="Belzedar94",
    url="https://github.com/Belzedar94/Spell-Stockfish",
    license="GPL-3.0-or-later",
    cmdclass={"build_ext": BuildExt},
    ext_modules=[Extension("pyffish_spell", sources=SOURCES, define_macros=MACROS,
                           include_dirs=["src"], language="c++")],
)
