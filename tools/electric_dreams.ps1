# Copies the tropical part of Epic's free Electric Dreams Environment sample into the game (look pass L2, L3).
# The sample must already be on the PC (Fab: "Electric Dreams Environment", Create Project, engine 5.8).
# Assets keep their /Game paths, so the copied plants find their materials without any fix-up.
# Everything copied is gitignored; assets/manifest.json has the reinstall line. Re-running only copies what changed.
#
#   powershell -File tools\electric_dreams.ps1 [-Sample "<path to ElectricDreamsEnv>"] [-Gentle]
#
# -Gentle waits between copied blocks (robocopy /IPG) so a big disk transfer running at the same time is not slowed.
param(
    [string]$Sample = "$env:USERPROFILE\OneDrive\Documents\Unreal Projects\ElectricDreamsEnv",
    [switch]$Gentle
)
$ErrorActionPreference = 'Stop'
$src = Join-Path $Sample 'Content'
$dst = Join-Path $PSScriptRoot '..\game\Content' | Resolve-Path
if (-not (Test-Path $src)) { throw "Electric Dreams sample not found at $Sample (see docs/plans/art-stage.md, 'What sneed does')" }

# Folder lists. Plants: tropical understorey, grass, ground cover and young palms (the palms also stand in for
# tree palms until L6). Surfaces and decals: jungle ground, swamp water, roots and debris for L3.
$plants = @(
  'AlexandraPalm','ArecaPalm','BroadleafLadyPalm','CanePalm','CatPalm','FanPalm','ZamiaPalm',
  'BirdOfParadise','BostonFern','Fern','SilverLadyFern','Taro','JungleGinger','ArrowheadPlant','CastorOilPlant',
  'PinkCordyline','VariegatedCroton','DragonTree','JanetCraigDracaena','MoneyPlant','Periwinkle',
  'KikuyuGrass','GroundCover','CloverVarieties','Crownbeard','WhiteWindflower','CommonPurslane',
  'CustomMoss','DeadLeaves','Lichen','FoliageInteraction'
)
$surfaces = @('JungleGround','MossyGrass','MossyRockyGround','SwampWater','ButtressRoot','JandBark','DriedGrassOnDampSoil','RockCliff','BeachCliff')
$decals = @('JungleDebris','JungleDebrisPile','GreenVine','DriedBambooPieces','ScatteredSticksAndTwigs','TileableMossPatches')

$folders = @()
$folders += $plants   | ForEach-Object { "Megascans\3D_Plants\$_" }
$folders += $surfaces | ForEach-Object { "Megascans\Surfaces\$_" }
$folders += $decals   | ForEach-Object { "Megascans\Decals\$_" }
# Master materials and the functions and textures they use (found by walking the packages' /Game imports).
$folders += 'Custom\CustomMaterials\Materials\Master_Materials\Custom_MS_Master'
$folders += 'Custom\CustomMaterials\Materials\Material_Functions'
$folders += 'MSPresets'

$ipg = @()
if ($Gentle) { $ipg = @('/IPG:20') }
$total = 0
foreach ($f in $folders) {
    $from = Join-Path $src $f
    if (-not (Test-Path $from)) { Write-Warning "missing in the sample: $f"; continue }
    $to = Join-Path $dst $f
    & robocopy $from $to /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 @ipg | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed on $f (exit $LASTEXITCODE)" }
    $total += (Get-ChildItem $to -Recurse -File | Measure-Object Length -Sum).Sum
}
"Electric Dreams tropical set in game\Content: {0:N2} GB in {1} folders" -f ($total / 1GB), $folders.Count
