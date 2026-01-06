# TabWindSwitcher

`TabWindSwitcher` is a utility for Windows that enhances the standard Alt+Tab window switching experience, bringing a more Linux-like functionality to your desktop. It provides an alternative method for navigating between open applications and their windows.

## Features

-   **Linux-style Alt+Tab replacement:** This tool replaces the default Windows Alt+Tab behavior with a more versatile switcher.
-   **Alt+~ (Alt + Tilde):** When you press `Alt + ~`, the switcher will display and allow you to select **all currently running programs** (applications). This means you can switch between different instances of the same application (e.g., multiple Notepad windows, multiple browser windows).
-   **Alt+Tab:** When you press `Alt + Tab`, the switcher will display and allow you to select **only the main application windows**, effectively grouping instances of the same application. This mimics the behavior often found in some Linux desktop environments where Alt+Tab cycles through applications rather than individual windows.

## Screenshot

Here's a preview of `TabWindSwitcher` in action:

![TabWindSwitcher Screenshot](assets/1.jpg)

## How it Works

The program intercepts the standard Alt+Tab shortcut and provides its own overlay for window selection. It distinguishes between individual window instances and main application groups, offering two distinct switching modes via `Alt+~` and `Alt+Tab`.

## Setup and Configuration

### `TabWindSwitcher.ini`

The `TabWindSwitcher.ini` file, located in the same directory as the executable, can be used to configure various aspects of the program (e.g., keybindings, visual settings, behavior). It allows for personalized adjustments to the switcher's functionality.

### Autostart with Windows

To have `TabWindSwitcher` launch automatically when Windows starts, you can add a shortcut to your Startup folder:

1.  Press `Win + R` to open the Run dialog.
2.  Type `shell:startup` and press Enter. This will open the Windows Startup folder.
3.  Create a shortcut to `TabWindSwitcher.exe` in this folder. You can do this by right-clicking on `TabWindSwitcher.exe`, selecting "Create shortcut," and then moving the created shortcut into the opened Startup folder.

## Installation and Usage

*(As this is a compiled executable, typical usage would involve running `TabWindSwitcher.exe`. Further installation instructions, if any, would be placed here.)*

## Building from Source

*(Information on how to compile `TabWindSwitcher.cpp` using `build.bat` or other means would be placed here.)*
