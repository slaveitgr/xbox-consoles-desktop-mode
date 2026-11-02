# Wine appearance foundation

These original files are intended for the actual guest Wine prefix.
`ProtonDesktop.reg` supplies a pale window/menu palette and blue selection color.
`ProtonDesktop.theme` records the same colors in the Windows theme INI format.
`wallpaper.svg` is original artwork, to be converted to a Wine-supported bitmap
when preparing a qualified rootfs. No fonts or Microsoft artwork are included.

None has been imported or tested in this project. The native UWP status screen
does not apply them or impersonate a Wine desktop. Rounded window chrome,
centered taskbar/Start and modern Explorer styling require a compatible Wine
theme or source patches after real Explorer boot works. Stock Wine colors alone
do not implement those features. MihawkOS/Wine patches may be assessed later
with their source and license notices; they are not copied into this starter.
