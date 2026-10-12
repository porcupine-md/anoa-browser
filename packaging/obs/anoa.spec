# anoa — RPM recipe for openSUSE and Fedora, built against the distribution's
# own Qt rather than a bundled copy.
#
# Every other channel this project ships (AppImage, portable tarball, Homebrew,
# the container) carries its own Qt. A distro package must not: the whole point
# is that the distribution owns the Qt, patches it, and updates it underneath
# us. CMakeLists.txt has a PORTABLE_RPATH option that exists precisely so this
# build can turn the bundle's $ORIGIN/lib RPATH off.
#
# The two distributions name Qt's packages differently, which is most of what
# the conditionals below are for.

Name:           anoa
Version:        {{VERSION}}
Release:        0
Summary:        A real browser that takes commands
License:        MIT
URL:            https://github.com/porcupine-md/anoa-browser
Source0:        https://github.com/porcupine-md/anoa-browser/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.16
BuildRequires:  gcc-c++

%if 0%{?suse_version}
BuildRequires:  qt6-base-devel >= 6.4
BuildRequires:  qt6-webengine-devel >= 6.4
BuildRequires:  qt6-websockets-devel >= 6.4
# Qt6PrintSupport is a WebEngineWidgets dependency, and its absence surfaces as
# the misleading "Failed to find WebEngineWidgets" rather than as itself.
BuildRequires:  cups-devel
%endif

%if 0%{?fedora} || 0%{?rhel}
BuildRequires:  qt6-qtbase-devel >= 6.4
BuildRequires:  qt6-qtwebengine-devel >= 6.4
BuildRequires:  qt6-qtwebsockets-devel >= 6.4
BuildRequires:  cups-devel
%endif

# What rpm cannot work out for itself.
#
# Much less than the container image needs, and the difference is the whole
# point of linking system Qt. Dockerfile installs some two dozen Chromium
# libraries because it unpacks a tarball with Qt bundled inside it and nothing
# declares a dependency on anything. Here the distribution's own qt6-webengine
# package carries those, and rpm derives the rest from DT_NEEDED.
#
# What is left is what nothing links and nothing declares: a font. Without one
# nothing renders as text and screenshots come back as boxes, which reads as a
# broken browser rather than a missing font.
%if 0%{?suse_version}
Requires:       dejavu-fonts
%endif
%if 0%{?fedora} || 0%{?rhel}
Requires:       dejavu-sans-fonts
%endif

%description
anoa is Chromium in one binary, driven from a shell, an HTTP endpoint, any CDP
client, or over MCP. There is no driver to install alongside it and no
headless-versus-real distinction to reason about.

The browser is the session: every command attaches to a browser that is already
running and leaves it running, so the page, the cookies and the scroll position
survive between commands.

%prep
%autosetup -n anoa-browser-%{version}

%build
# Two flags, both of which a packager has to get right and neither of which is
# the default.
#
# PORTABLE_RPATH=OFF is the packager's switch. Left on, the installed binary
# carries an RPATH of $ORIGIN/lib naming a directory no package ships, and it
# does not start at all. See CMakeLists.txt, which says so where the option is
# defined.
#
# RelWithDebInfo rather than Release, and not for the debug symbols themselves:
# CMakeLists.txt adds a link-time `-s` under Release, which strips the binary
# before rpm can extract anything from it. The build then fails late, at
# packaging time, with "Empty %%files file debugsourcefiles.list" — a message
# that says nothing about the strip flag that caused it. RelWithDebInfo skips
# that branch, and a distribution expects a -debuginfo subpackage anyway.
%cmake \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DANOA_VERSION_OVERRIDE=%{version} \
    -DPORTABLE_RPATH=OFF
%cmake_build

%install
%cmake_install

%check
# Enough to prove it links against the system Qt and starts. Anything that
# renders a page belongs outside the build root, where QtWebEngine's zygote can
# have the namespaces it wants. offscreen because --version still constructs the
# application object and a build host has no display.
QT_QPA_PLATFORM=offscreen %{buildroot}%{_bindir}/anoa --version

%files
%doc README.md
%{_bindir}/anoa
%{_datadir}/applications/anoa.desktop
%{_datadir}/icons/hicolor/scalable/apps/anoa.svg
%license %{_datadir}/licenses/anoa/LICENSE

%changelog
* {{DATE_RPM}} Eka Tresna Irawan <anak10thn@gmail.com> - {{VERSION}}-0
- Release {{VERSION}}. See the GitHub release notes for what changed.
