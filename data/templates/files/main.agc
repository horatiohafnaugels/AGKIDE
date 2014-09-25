
// Project: {project} 
// Created: {date}

// set window properties
SetWindowTitle( "{project}" )
SetScreenResolution( 1024, 768, 0 )

// set display properties
SetVirtualResolution( 1024, 768 )



do
    

    Print( ScreenFPS() )
    Sync()
loop
