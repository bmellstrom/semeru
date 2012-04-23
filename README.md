Semeru
======

Semeru is a small Java launcher intended to perform the following tasks:

* Provide a way to react to SIGINT, SIGTERM and optionally SIGHUP signals in a Java program.
* Enable setting specific Linux capabilities for the process before switching to another user.

The program is _not_ intended to be used to do anything that just as easily can be done from a
shell script, or to do something that the init system of choice can perform. For example, this
includes but is not limited to:

* Setup your classpath (use a shell script).
* Detach the process and keep it running in the background (use your init system).
* Setup logging destination (redirect stdout/stderr to a logger process before starting).

Java interface
--------------

Just like the normal java launcher, Semeru will try to find and execute a method with
the following signature in the main class:

    public static void main(String[] args)

The program will exit if/when this method returns. When a SIGINT or SIGTERM signal
is received, the following method will be executed in a separate thread:

    public static void shutdown()

Optionally, if a method with the following signature is defined it will be executed
when the SIGHUP signal is received:

    public static void reload()

Building and installing
-----------------------

Just type 'make' and hope for the best. Tweak the Makefile as necessary if needed.
Specifically, you may have to modify the location to your JDK root.

No installation is provided as of yet. Just copy the resulting binary to where you want it.

Running
-------

The syntax for the program is printed if `semeru` is invoked without any parameters from
a shell, and should be familiar for those used to the `java` command.

Semeru links to `libjvm.so` so you need to make sure that is included in your library path.
This can for example be done as follows (tweak as necessary):

    export LD_LIBRARY_PATH=$JAVA_HOME/jre/lib/amd64/server

You will also probably need to supply a class path to the JVM. Example:

    ./semeru -Djava.class.path=examples Example1
