#!/usr/bin/env bash

# Assuming that path contains the built binaries.
# And this script is called from the root directory.
which scram_tests && which scram \
  && scram_tests \
  && nosetests -w ./tests/ \
  && nosetests -w ./scripts/ \
  && ./scripts/fault_tree_generator.py -b 200 -c 5 \
  && scram -g fault_tree.xml -o fault_tree_Autogenerated.dot \
  && dot -q -Tsvg fault_tree_Autogenerated.dot -o fault_tree.svg \
  && rm fault_tree.xml fault_tree.svg fault_tree_Autogenerated.dot

exit $?
