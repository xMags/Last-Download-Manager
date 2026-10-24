# Queues and the Scheduler

A queue is a list of downloads that LDM works through by itself, a few at a time, when you start
it or when its schedule says so.

## The queues

- **Main download queue:** where **Download Later** puts a download, and where a batch or the
  Download All Links dialog can put its downloads. LDM asks which queue to use and whether to start
  it; choose "Don't ask again" to stop asking.
- **Synchronization queue:** for files that change on the server. When it runs, LDM asks the server
  whether each finished file changed and downloads it again only if it did. Unchanged files show
  "Unchanged on the server" in Properties.
- **Your own queues:** make one in the Scheduler (**New queue...**) or with **Add to queue > Create
  new queue** in a download's right-click menu.

The **Queues** node of the category tree lists every queue's downloads; click a queue to see only
its own, in queue order. The **Q** column marks downloads that are in a queue; point at the mark to
see which.

## Adding and removing downloads

- Right-click downloads and choose **Add to queue** and a queue, or **Delete from queue**.
- A download that is already complete is downloaded again when an ordinary queue reaches it, so
  LDM asks first. Choose No to add only the downloads that are not complete.
- In the Scheduler, **Files in the queue** lists a queue's downloads; **Move up** and **Move down**
  change the order, **Remove** takes them out of the queue (they stay in the list).

## Starting and stopping

- **Start queue** and **Stop queue** in the toolbar, the Downloads menu or a queue's right-click
  menu in the tree, or **Start now** and **Stop** in the Scheduler.
- A queue downloads as many files at the same time as **Download at the same time** says (1 to
  16), and tries a failed file again up to **Retries for each failed file** times, after the others.
- A queue stops by itself when nothing is left to download. Stopping it yourself pauses its
  downloads.
- `ldm.exe --start-queue` starts the main queue, also in an LDM that is already running.

## Schedules

On the **Schedule** page of the Scheduler:

- **Start the queue when LDM starts.**
- **Start the queue at** a time, **once** on a day or **daily** on chosen weekdays, optionally
  **again every** some minutes until the stop time or midnight.
- **Stop the queue at** a time; a stop time earlier than the start means the next day.
- **When the queue is done:** exit LDM, sleep, hibernate or turn off the computer. A countdown lets
  you cancel; nothing happens after a queue that you stopped yourself, or one that downloaded
  nothing.
- **Open this file when done:** a program or document to open when the queue finishes.

LDM has to be running at these times; in the notification area is enough. A start that passes while
LDM is closed is skipped, not caught up. LDM can wake a sleeping computer for a scheduled start
when Windows allows wake timers (Power Options > Sleep > Allow wake timers).

## Download limits

The **Download limits** page sets the download quota: LDM stops downloading after a number of
megabytes in a number of hours, optionally asking first. It is the same setting as in Options >
Connection and applies to every download.

## Site Grabber projects

Site Grabber projects follow the queues in the Scheduler's list. A project has only a schedule, and
what it does when its time comes: explore the site, explore and then download the checked files, or
download the checked files. A project whose window or settings are open, or that is already running,
is not started by its schedule. See [Site Grabber](site-grabber.md).
