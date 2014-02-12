#!/usr/bin/env python
import re
from os import popen, system

input  = "ice_bib"
bbl    = "texput.bbl"
output = "references.md"

header = """References {#references}
==========

@par Notes
This large list collects all references which the PISM authors have found
convenient.  There is no claim that all of these references get direct use,
or even mention, in the PISM project files.<br><br><hr>
"""

# dummy LaTeX document that \cites everything and uses a special BibTeX style file:
latexdummy = """\\documentclass{article}
\\begin{document}
\\cite{*}\\bibliography{%s}\\bibliographystyle{doxybib}
\\end{document}
""" % input

# Remove an old .bbl so that LaTeX does not choke on it:
system("rm -f %s" % bbl)

# Run LaTeX:
f= popen("latex", 'w')
f.write(latexdummy)
f.close()

# Run BibTeX:
system("bibtex texput")

# Read all the lines from a .bbl generated by BibTeX
f = open(bbl)
lines = f.readlines()
f.close()
body = "".join(lines[:])

# NB! The order of substitutions is important.
subs = [(r"%\n",                      r""), # lines wrapped by BibTeX
        (r"\\href{([^}]*)}{([^}]*)}", r'[\2](\1)'), # hyperref href command
        (r"\\url{([^}]*)}",           r'[\1](\1)'), # hyperref url command
        (r"\\\w*{([^}]*)}",           r" \1 "),                # ignore other LaTeX commands
        (r"[}{]",                     r""),                    # curly braces
        (r"\$\\sim\$",                r"~"),                   # LaTeX \sim used to represent ~
        (r"---",                      r"&mdash;"),             # em-dash
        (r"--",                       r"&ndash;"),             # en-dash
        (r"([^/])~",                  r"\1&nbsp;"),            # tildes that are not in URLs
        (r'\\"([a-zA-Z])',            r"&\1uml;"),             # umlaut
        (r"\\'([a-zA-Z])",            r"&\1grave;"),           # grave
        (r'\\`([a-zA-Z])',            r"&\1acute;"),           # acute
        (r'\\^([a-zA-Z])',            r"&\1circ;"),            # circumflex
        (r'``',                       r'"'),                   # opening quotes
        (r"''",                       r'"'),                   # closing quotes
        (r"\\,",                      r""),                    # \, LaTeX math spacing command
        (r"\\ae",                     r"&aelig;"),             # ae ligature
        (r"\\tt",                     r"\\c"),                 # \tt (in the 'siple' entry)
        ]

for (regex, substitution) in subs:
    body = re.compile(regex).sub(substitution, body)

f = open(output, 'w')
f.write(header)
f.write(body)
f.close()
