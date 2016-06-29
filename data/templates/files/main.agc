
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


do
    

    Print( ScreenFPS() )
    Sync()
loop
