error = "Hypha runtime is installed.\n"
error += "\n"
error += "This binary has no application embedded.\n"
error += "Build an app with: rake compile[path/to/app.rb]\n"
error += "See: https://github.com/Asmod4n/hypha-mrb"
raise NotImplementedError, error