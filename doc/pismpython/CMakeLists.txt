find_package(Sphinx)

if (NOT SPHINX_EXECUTABLE)

  message (STATUS "Sphinx was not found. Please install it (sphinx-doc.org) if you want to build the pismpython documentation.")

else ()

  set(RST_FILES
    index.rst 
    installation.rst 
    python.rst
    zbib.rst
    inverse/index.rst
    inverse/refresher.rst
    inverse/pism_ssa.rst
    inverse/ssa_forward.rst
    inverse/ssa_inverse.rst
    inverse/pismi.rst
    inverse/design.rst
    inverse/listeners.rst
    api/index.rst
    api/PISM.rst
    api/options.rst
    api/logging.rst
    api/model.rst
    api/util.rst
    api/vec.rst
    api/ssa.rst
    api/sia.rst
    api/invert/index.rst
    api/invert/ssa.rst
    api/invert/ssa_siple.rst
    api/invert/sipletools.rst
    api/invert/ssa_tao.rst
    )

  set (sphinx_helpers
    _static/pymunk.js
    pismdoc/theme.conf
    pismdoc/static/PISM.css )

  # Copy the bibliography file
  configure_file (../ice-bib.bib ice-bib.bib COPYONLY)

  # copy all the files needed to generate browser sources
  foreach (filename ${sphinx_helpers})
    configure_file(${filename} ${filename} COPYONLY)
  endforeach()

  foreach (filename ${RST_FILES})
    configure_file(${filename} ${filename} COPYONLY)
  endforeach()

  configure_file(conf.py conf.py @ONLY)
  configure_file(pism-sphinx-macros.tex pism-sphinx-macros.tex COPYONLY)

  add_custom_target (pismpython_docs
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/_static ${CMAKE_CURRENT_BINARY_DIR}/_static
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/site-packages ${CMAKE_BINARY_DIR}/site-packages

    COMMAND ${SPHINX_EXECUTABLE} -b html . html
    DEPENDS ${RST_FILES} ${sphinx_helpers} install_site_packages_in_the_build_directory
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} )


endif()


