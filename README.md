# D++ Local Music Bot (Pickle Rick)

This is a basic streaming bot for Discord powered by the [D++ library](https://dpp.dev) which streams a local MP3 collection from a directory. It has a secific use case and there are no plans to add streaming of YouTube etc to this bot (but you can fork this project and do what you want with it).

## Compilation

    mkdir build
    cd build
    cmake ..
    make -j

If DPP is installed in a different location you can specify the root directory to look in while running cmake 

    cmake .. -DDPP_ROOT_DIR=<your-path>

## Running the template bot

Create a config.json in the directory above the build directory:

```json
{
"token": "your bot token here", 
"homedir": "/path/to/mp3s",
"homeserver": "server id of server where the bot should run"
}
```

You should then make a file called `songindex.txt` within the mp3 directory which contains a list of relative filenames of all MP3 files within that directory. You can build such a file with a command like `cd /path/to/mp3s; find . -name '*.mp3'`. When the bot is running, updating this text file immediately updates the index of available songs.

Start the bot:

    cd build
    ./templatebot

