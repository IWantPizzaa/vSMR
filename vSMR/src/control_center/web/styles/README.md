# Control Center styles

`styles.css` is generated from the ordered source files in this directory by
`vSMR/tools/build_control_center_styles.ps1`.

The files are ordered cascade stages. Keep component rules in the narrowest
applicable source file, but do not reorder stages without checking the Control
Center in every page and compact layout. Run the generator after editing a
source file and commit the generated bundle with the source change.
