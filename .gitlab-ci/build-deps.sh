#!/bin/bash
#
# Builds the dependencies required for any OS/architecture combination. See
# .gitlab-ci.yml for more information. This script is called from an
# OS-specific build scripts like debian-install.sh.

source "${FDO_CI_BASH_HELPERS}"

set -o xtrace -o errexit

# Set concurrency to an appropriate level for our shared runners, falling back
# to the conservative default form before we had this variable.
export MAKEFLAGS="-j${FDO_CI_CONCURRENT:-4}"
export NINJAFLAGS="-j${FDO_CI_CONCURRENT:-4}"

# When calling pip in newer versions, we're required to pass
# --break-system-packages so it knows that we did really want to call pip and
# aren't just doing it by accident.
PIP_ARGS="--user"
case "$FDO_DISTRIBUTION_VERSION" in
  bullseye)
    ;;
  *)
    PIP_ARGS="$PIP_ARGS --break-system-packages"
    ;;
esac

# Build and install Meson. Generally we want to keep this in sync with what
# we require inside meson.build.
fdo_log_section_start_collapsed install_meson "install_meson"
pip3 install $PIP_ARGS git+https://github.com/mesonbuild/meson.git@1.4.2
export PATH=$HOME/.local/bin:$PATH

# Our docs are built using Sphinx (top-level organisation and final HTML/CSS
# generation), Doxygen (parse structures/functions/comments from source code),
# Breathe (a bridge between Doxygen and Sphinx), and we use the Read the Docs
# theme for the final presentation.
pip3 install $PIP_ARGS sphinx==4.2.0
pip3 install $PIP_ARGS sphinxcontrib-applehelp==1.0.4
pip3 install $PIP_ARGS sphinxcontrib-devhelp==1.0.2
pip3 install $PIP_ARGS sphinxcontrib-htmlhelp==2.0.0
pip3 install $PIP_ARGS sphinxcontrib-jsmath==1.0.1
pip3 install $PIP_ARGS sphinxcontrib-qthelp==1.0.3
pip3 install $PIP_ARGS sphinxcontrib-serializinghtml==1.1.5
pip3 install $PIP_ARGS breathe==4.31.0
pip3 install $PIP_ARGS sphinx_rtd_theme==1.0.0
fdo_log_section_end install_meson


# Build a Linux kernel for use in testing. We enable the VKMS module so we can
# predictably test the DRM backend in the absence of real hardware. We lock the
# version here so we see predictable results.
#
# To run this we use virtme-ng, a QEMU wrapper. It is a fork from virtme, whose
# development stalled.
#
# virtme-ng makes our lives easier by abstracting handling of the console,
# filesystem, etc, so we can pretend that the VM we execute in is actually
# just a regular container.
fdo_log_section_start_collapsed install_kernel "install_kernel"
if [[ -n "$KERNEL_DEFCONFIG" ]]; then
	git clone --depth=1 --branch=v7.1 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git linux
	cd linux

	if [[ "${BUILD_ARCH}" = "x86-64" ]]; then
		LINUX_ARCH=x86
	elif [[ "$BUILD_ARCH" = "aarch64" ]]; then
		LINUX_ARCH=arm64
	else
		echo "Invalid or missing \$BUILD_ARCH"
		exit 1
	fi

	if [[ -z "${KERNEL_DEFCONFIG}" ]]; then
		echo "Invalid or missing \$KERNEL_DEFCONFIG"
		exit
	fi
	if [[ -z "${KERNEL_IMAGE}" ]]; then
		echo "Invalid or missing \$KERNEL_IMAGE"
		exit
	fi

	make ARCH=${LINUX_ARCH} ${KERNEL_DEFCONFIG}
	make ARCH=${LINUX_ARCH} kvm_guest.config
	./scripts/config \
		--enable CONFIG_DRM \
		--enable CONFIG_DRM_KMS_HELPER \
		--enable CONFIG_DRM_VKMS \
		--enable CONFIG_UDMABUF \
		--disable CONFIG_BT \
		--disable CONFIG_DRM_AMDGPU \
		--disable CONFIG_DRM_I915 \
		--disable CONFIG_DRM_MSM \
		--disable CONFIG_DRM_NOUVEAU \
		--disable CONFIG_DRM_XE \
		--disable CONFIG_MEDIA_SUPPORT \
		--disable CONFIG_SOUND \
		--disable CONFIG_USB_SUPPORT \
		--disable CONFIG_WIRELESS \
		--disable CONFIG_WLAN

	make ARCH=${LINUX_ARCH} oldconfig
	make ARCH=${LINUX_ARCH}

	cd ..
	mkdir /weston-virtme
	mv linux/arch/${LINUX_ARCH}/boot/${KERNEL_IMAGE} /weston-virtme/
	mv linux/.config /weston-virtme/.config
	rm -rf linux

	git clone --depth=1 --branch=v1.25 --recurse-submodules https://github.com/arighi/virtme-ng.git virtme
	cd virtme
	./setup.py install
	cd ..
fi
fdo_log_section_end install_kernel

# These dependencies must be installed from package in the LTS build
if [ x"$USE_UPGRADED_DEPENDENCIES" = "xy" ] ; then
	fdo_log_section_start_collapsed install_wayland "install_wayland"
	git clone --branch 1.25.0 --depth=1 https://gitlab.freedesktop.org/wayland/wayland
	cd wayland
	git show -s HEAD
	meson setup build --wrap-mode=nofallback -Ddocumentation=false
	ninja ${NINJAFLAGS} -C build install
	cd ..
	rm -rf wayland
fi

# Keep this version in sync with our dependency in meson.build. If you wish to
# raise a MR against custom protocol, please change this reference to clone
# your relevant tree, and make sure you bump $FDO_DISTRIBUTION_TAG.
git clone --branch 1.47 --depth=1 https://gitlab.freedesktop.org/wayland/wayland-protocols
cd wayland-protocols
git show -s HEAD
meson setup build --wrap-mode=nofallback -Dtests=false
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf wayland-protocols
fdo_log_section_end install_wayland

# Build and install our own version of libdrm.
# 2.4.130 is the first version defining DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE.
fdo_log_section_start_collapsed install_libdrm "install_libdrm"
git clone --branch libdrm-2.4.130 --depth=1 https://gitlab.freedesktop.org/mesa/drm.git
cd drm
meson setup build --wrap-mode=nofallback -Dauto_features=disabled \
	-Dvc4=disabled -Dfreedreno=disabled -Detnaviv=disabled
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf drm
fdo_log_section_end install_libdrm

# Build and install Vulkan-Headers with a defined version, mostly because
# the version in Debian 11 (bullseye) is too old to build vulkan-renderer.
fdo_log_section_start_collapsed install_vulkan_headers "install_vulkan_headers"
git clone --branch sdk-1.3.239.0 --depth=1 https://github.com/KhronosGroup/Vulkan-Headers
cd Vulkan-Headers
cmake -G Ninja -B build
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf Vulkan-Headers
fdo_log_section_end install_vulkan_headers

# Build and install our own version of Mesa. Debian provides a perfectly usable
# Mesa, however llvmpipe's rendering behaviour can change subtly over time.
# This doesn't work for our tests which expect pixel-precise reproduction, so
# we lock it to a set version for more predictability. If you need newer
# features from Mesa then bump this version and $FDO_DISTRIBUTION_TAG, however
# please be prepared for some of the tests to change output, which will need to
# be manually inspected for correctness.
fdo_log_section_start_collapsed install_mesa "install_mesa"

# Needed for Mesa >= 25.3
git clone --branch 12.2.0 --depth=1 https://github.com/KhronosGroup/glslang
cd glslang
cmake -G Ninja -B build
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf glslang

git clone --branch mesa-26.1.2 --depth=1 https://gitlab.freedesktop.org/mesa/mesa.git
cd mesa
meson setup build --wrap-mode=nofallback -Dauto_features=disabled \
	-Dgallium-drivers=llvmpipe -Dvulkan-drivers=swrast -Dvideo-codecs= \
	-Degl=enabled -Dgbm=enabled -Dgles2=enabled -Dllvm=enabled \
	-Dshared-glapi=enabled -Dglx=disabled
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf mesa
fdo_log_section_end install_mesa

# seatd lets us avoid the pain of open-coding TTY assignment within Weston.
# We use this for our tests using the DRM backend.
fdo_log_section_start_collapsed install_seatd "install_seatd"
git clone --depth=1 --branch 0.6.1 https://git.sr.ht/~kennylevinsen/seatd
cd seatd
meson setup build --wrap-mode=nofallback -Dauto_features=disabled \
	-Dlibseat-seatd=enabled -Dlibseat-logind=systemd -Dserver=enabled
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf seatd
fdo_log_section_end install_seatd

# Build and install aml and neatvnc, which are required for the VNC backend
fdo_log_section_start_collapsed install_aml_neatvnc "install_aml_neatvnc"
git clone --branch v1.0.0 --depth=1 https://github.com/any1/aml.git
cd aml
meson setup build --wrap-mode=nofallback
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf aml
git clone --branch v1.0.0 --depth=1 https://github.com/any1/neatvnc.git
cd neatvnc
meson setup build --wrap-mode=nofallback -Dauto_features=disabled
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf neatvnc
fdo_log_section_end install_aml_neatvnc

# Build and install libdisplay-info, used by drm-backend
fdo_log_section_start_collapsed install_libdisplay-info "install_libdisplay-info"
git clone --branch 0.3.0 --depth=1 https://gitlab.freedesktop.org/emersion/libdisplay-info.git
cd libdisplay-info
# We need nodownload to prevent v4l-utils from being checked out, it breaks our 32-bit builds.
meson setup build --wrap-mode=nodownload
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf libdisplay-info
fdo_log_section_end install_libdisplay-info

# Build and install lcms2, which we use to support color-management.
fdo_log_section_start_collapsed install_lcms2 "install_lcms2"
git clone --branch master https://github.com/mm2/Little-CMS.git lcms2
cd lcms2
git checkout -b snapshot lcms2.16
meson setup build --wrap-mode=nofallback
ninja ${NINJAFLAGS} -C build install
cd ..
rm  -rf lcms2
fdo_log_section_end install_lcms2
