# Connect Kiln KV to GitHub

The local codebase is `/Users/Rishi/Coding/Personal Projects/kiln-kv`. It has a local Git repository on branch `main` and an initial commit. No remote repository has been created or pushed.

## First upload

1. Sign in to GitHub and visit https://github.com/new.
2. Name the repository `kiln-kv`, choose visibility, and leave the options to add a README, .gitignore, and license unchecked. The local project already has its own files. Create the repository.
3. Copy its HTTPS URL from the Quick Setup page.
4. Open Terminal and run the commands below, replacing `YOUR_USERNAME` with your actual GitHub username:

```sh
cd "/Users/Rishi/Coding/Personal Projects/kiln-kv"
git remote add origin https://github.com/YOUR_USERNAME/kiln-kv.git
git remote -v
git push -u origin main
```

If HTTPS authentication asks for a password, use a GitHub personal access token rather than your GitHub account password. Do not put the token into a URL, code file, or command argument. An existing credential manager or GitHub Desktop sign-in can also handle authentication. If you already use GitHub SSH authentication, copy the repository's SSH URL instead of its HTTPS URL.

## Upload later changes

From the project folder:

```sh
make test
git status
git diff
git add src include tests docs README.md Makefile .gitignore
git diff --cached
git commit -m "Describe the change you made"
git push
```

Only commit intended source and documentation changes. `build/`, sample `.kv` database files, and `.DS_Store` are ignored. Git does not upload automatically: committing saves a local snapshot; pushing uploads commits to GitHub.

If you work from multiple computers, run `git pull --ff-only` before starting work when your working tree is clean. If Git reports diverged history, inspect it before merging; do not use force push as a routine sync step.

Official reference: [Adding locally hosted code to GitHub](https://docs.github.com/en/migrations/importing-source-code/using-the-command-line-to-import-source-code/adding-locally-hosted-code-to-github).
