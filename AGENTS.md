# Repository guidance

- Target retail Xbox One / One S / One X Developer Mode first; keep x64 Windows UWP as the development host. Do not use WinUI 3, WPF, Windows Forms, or desktop-only process/window APIs in the app.
- Keep the desktop shell separate from the experimental executable runtime. Internal desktop windows are panels in one UWP application.
- Report Windows build, host tests, and physical Xbox tests separately. Never claim game compatibility based on a parser test or a desktop run.
- Prefer small self-authored diagnostic programs before adding third-party binaries. Do not bundle Steam credentials, commercial games, Microsoft artwork, Wine/Proton binaries, or code copied from reference projects without their licenses.
- Use C++17 for portable code, C++/WinRT for the UWP host, Direct3D 11 + Direct2D/DirectWrite for rendering, and Windows.Gaming.Input for controllers.
- Executable memory must transition from writable to executable; never request writable and executable pages simultaneously.
- Changes to runtime parsing or memory handling require meaningful automated checks. UI copy/style changes do not need tests that duplicate their implementation.
