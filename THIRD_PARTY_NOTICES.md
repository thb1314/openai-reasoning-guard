# Third-Party Notices

This repository bundles a small set of third-party UI and font assets so the
Qt/C++ application can build and package reproducibly.

Bundled components:

- `qt-cpp-client/third_party/qui/`: Qt Widgets UI helper code, stylesheets,
  icons, translations, and Font Awesome webfont resources used by the GUI.
- `qt-cpp-client/third_party/fonts/`: runtime fonts bundled for predictable
  Chinese UI rendering in local builds and Linux packages.

Project source code outside `third_party/` is released under the MIT License
in `LICENSE`. Third-party files remain under their respective upstream
licenses and copyright notices.
