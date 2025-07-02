GrahaOS is an AI integrated operating system built from scratch, it uses a custom designed protocol called the Operating System Control Protocol, for GrahaOS internally, it is referred to as Grahaos Control Protocol (GCP)

the idea is to completely simplify how AI actualyl performs actions on the OS, here is how it works:

1) An entire snapshot of the OS is taken, and any major register locations as well as debug messages, current state etc. are captured, this is only possible becuase GrahaOS is being built from scratch so the snapshot pipeline design is completely in our hands, the final snapshot is stored in a <.osm> file

2) the snapshot is simply sent to an AI externally via API (network drivers to be implemented) 

3) This is when the GCP comes in, using the <.osm> file as well as the predefined tools (combination of syscalls which are predefined, the AI simply has to call them in any order, you can think of them as a set of macros as well as individual syscalls dedicated for Ai itself) I have gone with this approach to increase speed as well as provide security such that the AI does not ever gain full system wide access via the original system syscalls.

4) the Ai will then return a json which contains the set of instructions to be executed in order.

In my opinion this is the most bearbones and simpelest way to implement OS level AI integration especially with an external AI.

I welcome any contributers, the vision for GrahaOS is to one day have people not even know how a keyboard and mouse works, they will simply talk to the computer and have it do things, converse, perform tasks and behave like a human whose thoughts can be visualised!

Technical vision: The next set of things I would want to do, is obviously implement a full scale system, but it should be designed in such a way that the right things are abstracted such that the snapshot is always refer to the system in full but is never too large. I would also like to integrate a local SLM that does things like memory management, page management etc by simply learning from user actions.




It is imperative to note that as of now two large files needed for compilation are not included here, one is the toolchain we are using, its just gnu binutils with gold 2.x and gcc compiler 15.x.x

Authors Notes:

I thought this was a grand idea and that it would be completely unique, but I have seen that people did come up with AI integrated OS (and it was expected to be honest), the most promising being an SDK framework that was developed by researchers from Rutgers university (its called AIOS), I have not seen anything at that scale yet, but hopefully GrahaOS still maintains a reasonable USP. I will be working on this project, simply because I love what I am doing whether it is unique or not!