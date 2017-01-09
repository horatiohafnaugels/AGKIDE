
// Project: {project} 
// Created: {date}

// show all errors
SetErrorMode(2)

// set window properties
SetWindowTitle( "{project}" )
SetWindowSize( 1024, 768, 0 )

// set display properties
SetVirtualResolution( 1024, 768 )
SetOrientationAllowed( 1, 1, 1, 1 )
SetSyncRate( 30, 0 ) // 30fps instead of 60 to save battery
UseNewDefaultFonts( 1 ) // since version 2.0.22 we can use nicer default fonts



do
    

    Print( ScreenFPS() )
    Sync()
loop
