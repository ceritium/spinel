class Item
  attr_reader :name, :price
  def initialize(name, price)
    @name = name
    @price = price
  end
end

$items = [Item.new("pen", 3), Item.new("book", 12)]
puts "stocked #{$items.size}"
