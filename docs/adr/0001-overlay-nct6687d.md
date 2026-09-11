# Overlay the distro nct6687d package

We splice VRM into the Arch `nct6687d-dkms-git` tree so driver git updates keep landing. We do not vendor `nct6687d` as our own DKMS package with official `PATCH[#]`.

Vendoring would be the documented DKMS model, but it pins the driver and fights the distro package for `nct6687.ko`. Overlay keeps the caller we already have. The splice language can still be a committed `-p1`; the owner of `nct6687.c` stays the distro package.
