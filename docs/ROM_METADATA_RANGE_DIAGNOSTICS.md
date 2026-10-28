# super.img liblp metadata range diagnostic

If ROM Workspace shows `NCV_LIBLP_DIAG_V2`, the installed APK includes the detailed range diagnostic. It reports the partition entry, stored/resolved name, metadata slot, `first_extent_index`, `num_extents`, and total extent count.

The required range condition is:

`first_extent_index + num_extents <= total_extents`

The parser deliberately does not bypass this check. AOSP liblp rejects an out-of-range extent list too. If the diagnostic appears for every metadata copy, keep the reported values: they are needed to distinguish malformed metadata from an offset/layout interpretation bug.

If the UI still displays only `partition 'product_b' has invalid extent range` without the `NCV_LIBLP_DIAG_V2` marker or numeric values, the installed APK is not using this updated source (or another older copy of the parser is being built). Rebuild/install the APK from this project and rerun analysis.

No real vendor image was included with the project archive, so the ROM-specific cause cannot be confirmed from source alone. This patch improves diagnostics and does not claim to fix or boot the ROM.
