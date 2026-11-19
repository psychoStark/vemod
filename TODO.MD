# vemod todolist

*feature*
    - [ ] add option to export into self contained 64bit apk
    - [ ] add support for full screen


*fix*
    - [x] make the pip mode optional in settings
    - [x] show corresponding icon of the app running in the recent page, instead of showing the vemod icon
    - [x] ask me questions and finalise the license, dont miss out on any thing the app and the whole sourse code should be completly legal. and update the gitignore appropriately
    - [x] remove all the mention of propriety codes so we can avoid any legal loopholes, the project should be completely legal
    - [x] add build section in readme
    - [x] restrict vemod container apks to be installed inside vemod, eg.: user selects vemod to install the container instead of the package installer. we can retrict apks from installing if the package name starts with com.vemood.container_
    - [x] removing an app from vemod should remove its container from the actual system if theres any
    - [x] remove option for add to homescreen as add to app drawer already exists
    - [x] make the add to app drawer into add to apps. add make it dynamic, if the app isnt added yet, it should show add to apps, if its added already it should show remove from apps, which if tapped would uninstall the container
    - [x] make the icon background white instead of white - which is currently. the icon should look normal and blend in with the other apps, should not look weird, offset and everything, colors, etc