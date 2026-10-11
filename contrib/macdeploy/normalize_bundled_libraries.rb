#!/usr/bin/env ruby
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
# Run after macdeployqt and before signing. Only rewrite Homebrew references
# whose exact library is already bundled; never hide a missing dependency.
require 'open3'
require 'find'

app = File.expand_path(ARGV.fetch(0))
abort 'Expected an app bundle' unless File.directory?("#{app}/Contents/Frameworks")

def run(*args)
  out, err, status = Open3.capture3(*args)
  abort "#{args.first}: #{err}" unless status.success?
  out
end

Find.find(app) do |path|
  next unless File.file?(path) && !File.symlink?(path) && run('file', '-b', path).include?('Mach-O')
  library_id = run('otool', '-D', path).lines.drop(1).map(&:strip).find { |line| !line.empty? }
  dependencies = run('otool', '-L', path).lines.drop(1).map { |line| line.strip.split(' (').first }
  dependencies.select { |dep| dep.start_with?('/opt/homebrew/', '/usr/local/') }.each do |dep|
    relative = dep[/[^\/]+\.framework\/Versions\/[^\/]+\/[^\/]+$/] || File.basename(dep)
    target = "#{app}/Contents/Frameworks/#{relative}"
    abort "Still missing #{dep}" unless File.file?(target)
    replacement = "@executable_path/../Frameworks/#{relative}"
    if dep == library_id
      run('install_name_tool', '-id', replacement, path)
    else
      run('install_name_tool', '-change', dep, replacement, path)
    end
  end
end
