-- "Uninstall Fast Track Pro 24-bit" app, installed in /Applications/Utilities by the package.
-- Runs the bundled uninstall script (the same one the uninstaller package uses) as root.
set appTitle to "Uninstall Fast Track Pro 24-bit"
display dialog "Remove the Fast Track Pro 24-bit plug-in? The Fast Track Pro goes back to being the normal 16-bit macOS device." & return & return & "Sound stops for a few seconds while Core Audio restarts." buttons {"Cancel", "Uninstall"} default button "Uninstall" cancel button "Cancel" with title appTitle with icon caution
set uninstallScript to quoted form of (POSIX path of (path to resource "uninstall.sh"))
try
	do shell script uninstallScript with administrator privileges
on error errText number errNum
	if errNum is -128 then return -- password prompt cancelled
	display dialog "Uninstall failed: " & errText buttons {"OK"} default button "OK" with title appTitle with icon stop
	return
end try
display dialog "Fast Track Pro 24-bit was removed." buttons {"OK"} default button "OK" with title appTitle
