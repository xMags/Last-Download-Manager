# Site Grabber

The Site Grabber explores a web site from a start page, lists the files it links to, and downloads
the ones you check. It can also save the pages themselves, for reading offline.

Open it with **Tasks > Run site grabber**.

## Projects and templates

The first window lists your projects and templates.

- **New project...** opens the project wizard.
- **New from template...** opens the wizard with a template's settings.
- **Open** opens a project's window; **Settings...** opens its wizard again.
- **Rename...** and **Delete**. Deleting a project removes its list, and its downloads that were
  never added to the main list. Files already on disk stay.

## The wizard

1. **The project:** a name, the start page and what to grab. The choices preset the other pages:
   all pictures on the site; audio on the start page and one level below; all video on the site;
   all files on the start page and one level below; the whole site for offline browsing; the start
   page for offline browsing; or Custom. A start page typed without `http://` or `https://` gets
   `https://`.
   **Sign-in:** a user name and password for sites that ask for one, or **Use the cookies of a
   connected browser**: LDM asks your browser for the cookies it sends to the start page, so a site
   you are signed in to there lets LDM in too. The cookies are used for that run only.
2. **Where the files are saved:** in each file's category folder, all in one category, or all in
   one folder, optionally keeping the site's folders below it. **Save the pages too** saves every
   page explored, with links pointing at the saved copies, for offline browsing; with category
   folders, pages and files then go to the General category's folder. You can also replace files
   that already exist, and add the checked files to the main list when the window closes.
3. **Which pages are explored:** the whole site or a number of levels of links from the start page;
   how many levels to follow into other sites (0: none); whether to stay below the start page's
   folder; whether every site of the start page's domain counts as the same site; paths to explore
   or never explore (masks such as `/docs/` or `*/private/*`); how many pages to read at once.
4. **Which files are listed:** file types to list or leave out (masks such as `*.jpg`), a smallest
   and largest size, paths to include or leave out, only files on the start page's site, each
   address once, download the matching files while exploring, use the link text as the description,
   and how many files to download at once.

## The Grabber window

- **Explore** reads the site again from the start, with a fresh list. **Update all** reads it again
  and keeps what you checked. **Stop** stops exploring and downloading.
- **Download checked** downloads the checked files that are not downloaded yet; **Stop downloads**
  stops them. A file that fails is tried once more, then shows as failed.
- **Add to main list** shows the downloads of the checked files in LDM's main list. Until then a
  project's downloads appear only in its window.
- The tree on the left shows all files, the checked, downloaded and failed ones, the files of each
  type, the files on other sites, the files the filters left out, and the pages.
- Click a file's check box, or select files and press Space, to check or uncheck them. The
  right-click menu also checks or unchecks all, opens a downloaded file or its folder, and adds the
  selected files to the main list.
- **Statistics** shows how many pages were read and files found and downloaded.
- **Save as template** keeps the project's settings as a template for new projects.

The line at the bottom says what the project is doing, and why nothing was found when the start
page could not be read.

## Limits

- The Grabber reads pages as the server sends them and does not run their scripts, so links that
  only scripts create are not found.
- One exploration reads at most 50,000 pages and lists at most 200,000 addresses; pages over 8 MB
  are not read.
- A site on the internet cannot lead the Grabber to addresses in your home network.
- Sizes show once a file is downloaded, or as soon as a size filter needs them.
- The Grabber does not follow `robots.txt`; it fetches only what you could open yourself, at the
  pace you set.

## Schedules

A project can run by itself: see [Queues and the Scheduler](queues.md).
